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

// #include <app-common/zap-generated/attributes/Accessors.h>

// #include <zephyr/device.h>
// #include <zephyr/devicetree.h>
// #include <zephyr/drivers/adc.h>
// #include <zephyr/drivers/gpio.h>
// #include <math.h>

// #include <zephyr/sys/printk.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

//k_timer sSensorTimer;

// #if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
// #error "No suitable devicetree overlay specified"
// #endif

// TODO Move this to configuration, so they can be easily changed.
//
// #define THERMISTORNOMINAL 10000
// #define TEMPERATURENOMINAL 25
// #define BCOEFFICIENT 3977
// #define SERIESRESISTOR 10000

// #define DT_SPEC_AND_COMMA(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

// static const struct adc_dt_spec adc_channels[] = {
// 	DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels, DT_SPEC_AND_COMMA)
// };

// #define PROBE_1_DIVIDER_POWER_NODE DT_NODELABEL(probe_1_divider_power)
// #define PROBE_2_DIVIDER_POWER_NODE DT_NODELABEL(probe_2_divider_power)

// static const struct gpio_dt_spec probe_1_divider_power = GPIO_DT_SPEC_GET(PROBE_1_DIVIDER_POWER_NODE, gpios);
// static const struct gpio_dt_spec probe_2_divider_power = GPIO_DT_SPEC_GET(PROBE_2_DIVIDER_POWER_NODE, gpios);

// void SensorTimerHandler(k_timer *timer)
// {
//     Nrf::PostTask([] { AppTask::SensorMeasureHandler(); });
// }

CHIP_ERROR AppTask::Init()
{
	ReturnErrorOnFailure(Nrf::Matter::PrepareServer());

	if (!Nrf::GetBoard().Init()) {
		LOG_ERR("User interface initialization failed.");
		return CHIP_ERROR_INCORRECT_STATE;
	}

	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(Nrf::Board::DefaultMatterEventHandler, 0));

	// ConfigureGPIO();

	// // TODO This should only start on successfully connection to the network.
	// //
 	//k_timer_init(&sSensorTimer, &SensorTimerHandler, nullptr);
    //k_timer_user_data_set(&sSensorTimer, this);
	//k_timer_start(&sSensorTimer, K_MSEC(2000), K_MSEC(2000));

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

// uint16_t probe_1_ref_internal;
// uint16_t probe_2_ref_internal;

// void AppTask::ConfigureGPIO() 
// {
// 	int err;

// 	if (!gpio_is_ready_dt(&probe_1_divider_power))
// 	{
// 		LOG_ERR("Cannot configure Divider Power switch (err: %d)", err);
// 		return;
// 	}

// 	err = gpio_pin_configure_dt(&probe_1_divider_power, GPIO_OUTPUT_INACTIVE);
// 	if (err != 0)
// 	{
// 		LOG_ERR("Configuring Divider Power pin failed (err: %d)", err);
// 		return;
// 	}

// 	if (!gpio_is_ready_dt(&probe_2_divider_power))
// 	{
// 		LOG_ERR("Cannot configure Divider Power switch (err: %d)", err);
// 		return;
// 	}

// 	err = gpio_pin_configure_dt(&probe_2_divider_power, GPIO_OUTPUT_INACTIVE);
// 	if (err != 0)
// 	{
// 		LOG_ERR("Configuring Divider Power pin failed (err: %d)", err);
// 		return;
// 	}

// 	for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++)
// 	{
// 		if (!device_is_ready(adc_channels[i].dev))
// 		{
// 			printk("ADC controller device %s not ready\n", adc_channels[i].dev->name);

// 			LOG_ERR("ADC controller device not ready");
// 			return;
// 		}

// 		err = adc_channel_setup_dt(&adc_channels[i]);

// 		if (err < 0)
// 		{
// 			printk("Could not setup channel #%d (%d)\n", i, err);

// 			LOG_ERR("Could not setup channel #%d (%d)", i, err);
// 			return;
// 		}

// 		LOG_INF("Successfully setup ADC channel #%d", i);
// 	}
// }

// uint16_t adc_sequence_buf;

// struct adc_sequence adc_sequence = {
// 	.buffer = &adc_sequence_buf,
// 	.buffer_size = sizeof(adc_sequence_buf),
// 	//.calibrate = true,
// };

// int16_t read_probe_temperature(int probe_number)
// {
// 	int channel = probe_number - 1;

// 	adc_dt_spec adc_channel = adc_channels[channel];

// 	int err = adc_sequence_init_dt(&adc_channel, &adc_sequence);

// 	if (err < 0)
// 	{
// 		LOG_INF("Could initialise ADC%d (%d)", channel, err);
// 		return -1;
// 	}

// 	err = adc_read_dt(&adc_channel, &adc_sequence);

// 	if (err < 0)
// 	{
// 		LOG_INF("Could not read ADC%d (%d)", channel, err);
// 		return -1;
// 	}

// 	int32_t val_mv = (int32_t)adc_sequence_buf;

// 	err = adc_raw_to_millivolts_dt(&adc_channel, &val_mv);

// 	if (err < 0) {
// 	    LOG_ERR(" (value in mV not available)\n");
// 		return -1;
// 	} 

// 	uint16_t ref_internal = adc_ref_internal(adc_channel.dev);

// 	//float mv_per_lsb = ref_internal / 4096.0F;	

// 	//LOG_INF("mv_per_lsb %d", (int)mv_per_lsb);
// 	//LOG_INF("mv_per_lsb %d", val_mv);

// 	//int32_t val_mv = (float)adc_reading * mv_per_lsb;

// 	float resistance = (val_mv * SERIESRESISTOR) / (1800 - val_mv);

// 	double steinhart;
// 	steinhart = resistance / THERMISTORNOMINAL;		  // (R/Ro)
// 	steinhart = log(steinhart);						  // ln(R/Ro)
// 	steinhart /= BCOEFFICIENT;						  // 1/B * ln(R/Ro)
// 	steinhart += 1.0 / (TEMPERATURENOMINAL + 273.15); // + (1/To)
// 	steinhart = 1.0 / steinhart;					  // Invert
// 	steinhart -= 273.15;							  // Convert to Celcius

// 	int16_t value = (int16_t)(steinhart);

// 	LOG_INF("ADC CHANNEL %d", channel);
// 	LOG_INF("Reference %d mV", ref_internal);
// 	//LOG_INF("A: %d", adc_sequence);
// 	LOG_INF("V: %"PRId32" mV", val_mv);
// 	LOG_INF("R: %d", (int)resistance);
// 	LOG_INF("T: %d", value);

// 	return value;
// }

// void AppTask::SensorMeasureHandler()
// {
	// Switch on the power pins.
	//
	//gpio_pin_set_dt(&probe_1_divider_power, 1);
	//gpio_pin_set_dt(&probe_2_divider_power, 1);

	// Let the voltage stabalise.
	//
	//k_sleep(K_MSEC(1000));

	// Read the temperatures.
	//
	//int16_t probe_1_temperature = read_probe_temperature(1) * 100;
	//int16_t probe_2_temperature = read_probe_temperature(2);

	// Switch off the power pins.
	//
	//gpio_pin_set_dt(&probe_1_divider_power, 0);
	//gpio_pin_set_dt(&probe_2_divider_power, 0);

	// Store the values in the attributes.
	//
    //chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(1, probe_1_temperature);
    //chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(2, probe_2_temperature);
//     chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(1, 10000);
//     chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(2, 10000);
// }