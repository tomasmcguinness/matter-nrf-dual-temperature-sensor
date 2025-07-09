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

#include <dk_buttons_and_leds.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>

#include <math.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

k_timer sSensorTimer;

#define TEMPERATURE_DIVIDER_POWER_NODE DT_NODELABEL(temperature_divider_power)

static const struct gpio_dt_spec temperature_divider_power_gpio = GPIO_DT_SPEC_GET(TEMPERATURE_DIVIDER_POWER_NODE, gpios);

// ADC Configuration
//
#define DT_SPEC_AND_COMMA(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

static const struct adc_dt_spec adc_channels[] = {DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels, DT_SPEC_AND_COMMA)};

void SensorTimerHandler(k_timer *timer)
{
    Nrf::PostTask([] { AppTask::SensorMeasureHandler(); });
}

void StartSensorTimer(uint32_t aTimeoutMs)
{
    k_timer_start(&sSensorTimer, K_MSEC(aTimeoutMs), K_MSEC(aTimeoutMs));
}

void StopSensorTimer()
{
    k_timer_stop(&sSensorTimer);
}

void ConfigureGPIO(void)
{
	if (!gpio_is_ready_dt(&temperature_divider_power_gpio))
	{
		LOG_ERR("Divider power GPIO is not ready");
		return;
	}

	int err = gpio_pin_configure_dt(&temperature_divider_power_gpio, GPIO_OUTPUT_INACTIVE);
	if (err != 0)
	{
		LOG_ERR("Configuring divider power pin failed (err: %d)", err);
		return;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++)
	{
		if (!device_is_ready(adc_channels[i].dev))
		{
			LOG_ERR("ADC controller device not ready");
			return;
		}

		err = adc_channel_setup_dt(&adc_channels[i]);

		if (err < 0)
		{
			LOG_ERR("Could not setup channel #%d (%d)", i, err);
			return;
		}

		LOG_INF("Successfully setup channel #%d", i);
	}
}

int16_t temperature_buf;

struct adc_sequence temperature_sequence = {
	.buffer = &temperature_buf,
	.buffer_size = sizeof(temperature_buf),
	.calibrate = true,
};

#define THERMISTORNOMINAL 10000
#define TEMPERATURENOMINAL 25
#define BCOEFFICIENT 3977
#define SERIESRESISTOR 10000

// The ADC has 4096 levels. The reference volage depends on the gain
//
float mv_per_lsb = 3000.0F / 4096.0F;	   // 0.6 / (1/5) = 3

uint16_t ReadTemperature(int i)
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

CHIP_ERROR AppTask::Init()
{
	/* Initialize Matter stack */
	ReturnErrorOnFailure(Nrf::Matter::PrepareServer());

	if (!Nrf::GetBoard().Init()) {
		LOG_ERR("User interface initialization failed.");
		return CHIP_ERROR_INCORRECT_STATE;
	}

	/* Register Matter event handler that controls the connectivity status LED based on the captured Matter network
	 * state. */
	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(Nrf::Board::DefaultMatterEventHandler, 0));

	ConfigureGPIO();

   	k_timer_init(&sSensorTimer, &SensorTimerHandler, nullptr);
    k_timer_user_data_set(&sSensorTimer, this);

	return Nrf::Matter::StartServer();
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	while (true) {
		Nrf::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}

void AppTask::SensorActivateHandler()
{
    StartSensorTimer(1000);
}

void AppTask::SensorDeactivateHandler()
{
    StopSensorTimer();
}

void AppTask::SensorMeasureHandler()
{
	// Switch on the power pin.
	//
	gpio_pin_set_dt(&temperature_divider_power_gpio, 1);

	// Let the voltage stabalise.
	//
	k_sleep(K_MSEC(100));

	// Read the temperatures and adjust them to the correct scale.
	//
	uint16_t temperature_1 = ReadTemperature(0) * 100;
	uint16_t temperature_2 = ReadTemperature(1) * 100;

	// Switch off the power pin.
	//
	gpio_pin_set_dt(&temperature_divider_power_gpio, 0);

	// Write the temperatures to the attributes.
	//
    chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(1, temperature_1);
    chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(2, temperature_2);
}
