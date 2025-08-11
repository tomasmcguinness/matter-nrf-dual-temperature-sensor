/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"

#include "app/matter_init.h"
#include "app/task_executor.h"
#include "board/board.h"
#include "lib/core/CHIPError.h"
#include "lib/support/CodeUtils.h"

#include <setup_payload/OnboardingCodesUtil.h>

#include <zephyr/logging/log.h>

#include <app-common/zap-generated/attributes/Accessors.h>

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <math.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

k_timer sSensorTimer;

#define DT_SPEC_AND_COMMA(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

static const struct adc_dt_spec adc_channels[] = {DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels, DT_SPEC_AND_COMMA)};

#define THERMISTORNOMINAL 10000
#define TEMPERATURENOMINAL 25
#define BCOEFFICIENT 3977
#define SERIESRESISTOR 10000
#define CONFIG_REFERENCE_VOLTAGE 3300

int16_t temperature_buf;

struct adc_sequence temperature_sequence = {
	.buffer = &temperature_buf,
	.buffer_size = sizeof(temperature_buf),
	.calibrate = true,
};

float mv_per_lsb = 3000.0F / 4096.0F;	   // 0.6 / (1/5) = 3
//float batt_mv_per_lsb = 3600.0F / 4096.0F; // 0.6 / (1/6) = 3.6

void SensorTimerHandler(k_timer *timer)
{
	Nrf::PostTask([]
				  { AppTask::SensorMeasureHandler(); });
}

CHIP_ERROR AppTask::Init()
{
	/* Initialize Matter stack */
	ReturnErrorOnFailure(Nrf::Matter::PrepareServer());

	if (!Nrf::GetBoard().Init())
	{
		LOG_ERR("User interface initialization failed.");
		return CHIP_ERROR_INCORRECT_STATE;
	}

	/* Register Matter event handler that controls the connectivity status LED based on the captured Matter network
	 * state. */
	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(Nrf::Board::DefaultMatterEventHandler, 0));

	k_timer_init(&sSensorTimer, &SensorTimerHandler, nullptr);
	k_timer_user_data_set(&sSensorTimer, this);
	k_timer_start(&sSensorTimer, K_MSEC(5000), K_MSEC(5000));

	int err;

	for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++)
	{
		if (!device_is_ready(adc_channels[i].dev))
		{
			LOG_ERR("ADC controller device not ready");
			return CHIP_ERROR_INTERNAL;
		}

		err = adc_channel_setup_dt(&adc_channels[i]);

		if (err)
		{
			LOG_ERR("Could not setup channel #%d (%d)", i, err);
			return CHIP_ERROR_INTERNAL;
		}

		LOG_INF("Setup channel #%d", i);
	}

	return Nrf::Matter::StartServer();
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	while (true)
	{
		Nrf::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}

static uint16_t read_temperature(int i)
{
	int err = adc_sequence_init_dt(&adc_channels[i], &temperature_sequence);

	if (err < 0)
	{
		LOG_ERR("Could initialise ADC%d (%d)", i, err);
		return -1;
	}

	err = adc_read(adc_channels[i].dev, &temperature_sequence);

	if (err < 0)
	{
		LOG_ERR("Could not read ADC%d (%d)", i, err);
		return -1;
	}

	int32_t adc_reading = temperature_buf;

	int32_t val_mv = (float)adc_reading * mv_per_lsb;

	float reading = (val_mv * SERIESRESISTOR) / (CONFIG_REFERENCE_VOLTAGE - val_mv);

	double steinhart;
	steinhart = reading / THERMISTORNOMINAL;		  // (R/Ro)
	steinhart = log(steinhart);						  // ln(R/Ro)
	steinhart /= BCOEFFICIENT;						  // 1/B * ln(R/Ro)
	steinhart += 1.0 / (TEMPERATURENOMINAL + 273.15); // + (1/To)
	steinhart = 1.0 / steinhart;					  // Invert
	steinhart -= 273.15;							  // Convert to Celcius

	uint16_t value = (uint16_t)(steinhart);

	LOG_INF("ADC CHANNEL %d", i);
	LOG_INF("A: %d", adc_reading);
	LOG_INF("V: %d", val_mv);
	LOG_INF("R: %d", (int)reading);
	LOG_INF("T: %d", value);

	return value;
}

void AppTask::SensorMeasureHandler()
{
	LOG_INF("Updating temperatures...");

	chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(1, read_temperature(0));
	chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(2, read_temperature(1));
}
