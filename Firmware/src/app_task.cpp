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

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <math.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

k_timer sIndicatorTimer;
k_timer sSensorTimer;
bool mIndicatorState;

#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "No suitable devicetree overlay specified"
#endif

// TODO Move this to configuration (Maybe even Matter?), so they can be easily changed.
//
#define THERMISTORNOMINAL 10000
#define TEMPERATURENOMINAL 25
#define BCOEFFICIENT 3977
#define SERIESRESISTOR 10000

#define DT_SPEC_AND_COMMA(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

static const struct adc_dt_spec adc_channels[] = {
	DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels, DT_SPEC_AND_COMMA)};

#define PROBE_1_DIVIDER_POWER_NODE DT_NODELABEL(probe_1_divider_power)
#define PROBE_2_DIVIDER_POWER_NODE DT_NODELABEL(probe_2_divider_power)

#define INDICATOR_LED_NODE DT_ALIAS(indicator_led)
#define RESET_BUTTON_NODE DT_ALIAS(reset_button)

static const struct gpio_dt_spec probe_1_divider_power = GPIO_DT_SPEC_GET(PROBE_1_DIVIDER_POWER_NODE, gpios);
static const struct gpio_dt_spec probe_2_divider_power = GPIO_DT_SPEC_GET(PROBE_2_DIVIDER_POWER_NODE, gpios);

static const struct gpio_dt_spec indicator_led = GPIO_DT_SPEC_GET(INDICATOR_LED_NODE, gpios);
static const struct gpio_dt_spec reset_button = GPIO_DT_SPEC_GET(RESET_BUTTON_NODE, gpios);

static struct gpio_callback reset_button_cb_data;

void SensorTimerCallback(k_timer *timer)
{
	Nrf::PostTask([]
				  { AppTask::SensorMeasureHandler(); });
}

void IndicatorTimerCallback(k_timer *timer)
{
	LOG_INF("LED Indicator: %d", mIndicatorState);

	mIndicatorState = !mIndicatorState;

	gpio_pin_set_dt(&indicator_led, mIndicatorState);
}

void AppTask::MatterEventHandler(const ChipDeviceEvent *event, intptr_t data)
{
	static bool isNetworkProvisioned = false;
	static bool isBleConnected = false;

	switch (event->Type)
	{
		case DeviceEventType::kServiceProvisioningChange:
			LOG_INF("Provisioning changed!");
		break;
	case DeviceEventType::kCHIPoBLEAdvertisingChange:
		isBleConnected = ConnectivityMgr().NumBLEConnections() != 0;
		break;
	case DeviceEventType::kThreadStateChange:
	case DeviceEventType::kWiFiConnectivityChange:
		isNetworkProvisioned = ConnectivityMgrImpl().IsIPv6NetworkProvisioned() && ConnectivityMgrImpl().IsIPv6NetworkEnabled();
		break;
	default:
		break;
	}

	if (isNetworkProvisioned)
	{
		LOG_INF("Network is provisioned!");

		k_timer_stop(&sIndicatorTimer);

		gpio_pin_set_dt(&indicator_led, 0);

		k_timer_start(&sSensorTimer, K_MSEC(5000), K_MSEC(5000));
	}
	else if (isBleConnected)
	{
		LOG_INF("Bluetooth connection opened");

		k_timer_start(&sIndicatorTimer, K_MSEC(200), K_MSEC(200));
	}
	else if (ConnectivityMgr().IsBLEAdvertising())
	{
		LOG_INF("Bluetooth is advertising");

		k_timer_start(&sIndicatorTimer, K_MSEC(1000), K_MSEC(1000));
	}
	else
	{
		LOG_INF("Bluetooth is disconnected");

		k_timer_stop(&sSensorTimer);
		k_timer_stop(&sIndicatorTimer);
		k_timer_start(&sIndicatorTimer, K_MSEC(1000), K_MSEC(1000));
	}
}

CHIP_ERROR AppTask::Init()
{
	ReturnErrorOnFailure(Nrf::Matter::PrepareServer());

	k_timer_init(&sIndicatorTimer, &IndicatorTimerCallback, nullptr);
	k_timer_user_data_set(&sIndicatorTimer, this);

	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(AppTask::MatterEventHandler, 0));

	ConfigureGPIO();

	k_timer_init(&sSensorTimer, &SensorTimerCallback, nullptr);
	k_timer_user_data_set(&sSensorTimer, this);

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

void pin_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	LOG_INF("Reset Button Clicked");
	gpio_pin_toggle_dt(&indicator_led);

	chip::Server::GetInstance().ScheduleFactoryReset();
}

void AppTask::ConfigureGPIO()
{
	if (!gpio_is_ready_dt(&probe_1_divider_power))
	{
		LOG_ERR("Cannot configure Divider 1 Power");
		return;
	}

	int err = gpio_pin_configure_dt(&probe_1_divider_power, GPIO_OUTPUT_INACTIVE);
	if (err != 0)
	{
		LOG_ERR("Configuring Divider 1 Power pin failed (err: %d)", err);
		return;
	}

	if (!gpio_is_ready_dt(&probe_2_divider_power))
	{
		LOG_ERR("Cannot configure Divider 2 Power");
		return;
	}

	err = gpio_pin_configure_dt(&probe_2_divider_power, GPIO_OUTPUT_INACTIVE);
	if (err != 0)
	{
		LOG_ERR("Configuring Divider 2 Power pin failed (err: %d)", err);
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

		LOG_INF("Successfully setup ADC channel #%d", i);
	}

	if (!gpio_is_ready_dt(&indicator_led))
	{
		LOG_ERR("Cannot configure indicator LED");
		return;
	}

	LOG_ERR("Indicator LED is ready");

	err = gpio_pin_configure_dt(&indicator_led, GPIO_OUTPUT_INACTIVE);
	if (err != 0)
	{
		LOG_ERR("Configuring indicator pin failed (err: %d)", err);
		return;
	}

	LOG_INF("Successfully configured indicator LED");

	if (!gpio_is_ready_dt(&reset_button))
	{
		LOG_ERR("Cannot configure reset button");
		return;
	}

	gpio_pin_configure_dt(&reset_button, GPIO_INPUT);

	gpio_pin_interrupt_configure_dt(&reset_button, GPIO_INT_EDGE_BOTH);

	gpio_init_callback(&reset_button_cb_data, pin_isr, BIT(reset_button.pin));

	gpio_add_callback(reset_button.port, &reset_button_cb_data);

	LOG_INF("Successfully configured reset button");
}

uint16_t adc_sequence_buf;

struct adc_sequence adc_sequence = {
	.buffer = &adc_sequence_buf,
	.buffer_size = sizeof(adc_sequence_buf),
	.calibrate = true,
};

double read_probe_temperature(int probe_number)
{
	int channel = probe_number - 1;

	adc_dt_spec adc_channel = adc_channels[channel];

	int err = adc_sequence_init_dt(&adc_channel, &adc_sequence);

	if (err < 0)
	{
		LOG_INF("Could initialise ADC%d (%d)", channel, err);
		return -1;
	}

	err = adc_read_dt(&adc_channel, &adc_sequence);

	if (err < 0)
	{
		LOG_INF("Could not read ADC%d (%d)", channel, err);
		return -1;
	}

	int32_t val_mv = (int32_t)adc_sequence_buf;

	err = adc_raw_to_millivolts_dt(&adc_channel, &val_mv);

	if (err < 0)
	{
		LOG_ERR(" (value in mV not available)\n");
		return -1;
	}

	// uint16_t ref_internal = adc_ref_internal(adc_channel.dev);

	float resistance = (val_mv * SERIESRESISTOR) / (1800 /* Ref voltage of 900 with a GAIN of 1_2 */ - val_mv);

	double steinhart;
	steinhart = resistance / THERMISTORNOMINAL;		  // (R/Ro)
	steinhart = log(steinhart);						  // ln(R/Ro)
	steinhart /= BCOEFFICIENT;						  // 1/B * ln(R/Ro)
	steinhart += 1.0 / (TEMPERATURENOMINAL + 273.15); // + (1/To)
	steinhart = 1.0 / steinhart;					  // Invert
	steinhart -= 273.15;							  // Convert to Celcius

	double value = steinhart;

	LOG_INF("ADC CHANNEL %d", channel);
	// LOG_INF("Reference %d mV", ref_internal);
	// LOG_INF("A: %d", adc_sequence);
	LOG_INF("V: %" PRId32 " mV", val_mv);
	LOG_INF("R: %d", (int)resistance);
	LOG_INF("T: %f", value);

	return value;
}

void AppTask::SensorMeasureHandler()
{
	// Switch on the power pins.
	//
	gpio_pin_set_dt(&probe_1_divider_power, 1);
	k_sleep(K_MSEC(1000));
	int16_t probe_1_temperature = read_probe_temperature(1) * 100;
	gpio_pin_set_dt(&probe_1_divider_power, 0);

	gpio_pin_set_dt(&probe_2_divider_power, 1);
	k_sleep(K_MSEC(1000));
	int16_t probe_2_temperature = read_probe_temperature(2) * 100;
	gpio_pin_set_dt(&probe_2_divider_power, 0);

	chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(1, probe_1_temperature);
	chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(2, probe_2_temperature);
}