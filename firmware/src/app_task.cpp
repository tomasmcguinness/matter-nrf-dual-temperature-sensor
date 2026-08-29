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

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <math.h>

#include "DeviceInfoProviderImpl.h"
#include "battery.h"

#include <app-common/zap-generated/callback.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/clusters/power-source-server/power-source-server.h>

#ifdef CONFIG_CHIP_ENABLE_ICD_SUPPORT
#include <app/icd/server/ICDConfigurationData.h>
#include <app/icd/server/ICDStateObserver.h>
#include <app/server/Server.h>
#endif

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

k_timer sIndicatorTimer;
k_timer sSensorTimer;
k_timer sBatteryTimer;
k_timer sFactoryResetTimer;
k_timer sButtonDebounceTimer;
bool mIndicatorState;

// While the button is held the LED shows the button state, so the connectivity
// blink patterns must leave it alone.
//
bool sButtonPressed;

#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "No suitable devicetree overlay specified"
#endif

// Read the temperature sensors every minute.
#define SENSOR_READ_INTERVAL 60000 

// A coin cell's voltage moves over months, so a daily sample is ample resolution.
// The reading taken at start up covers cell replacement, which power cycles the
// device anyway.
//
#define BATTERY_READ_INTERVAL_HOURS 24

// The button is a bare mechanical switch with no hardware debouncing, so an
// edge is only believed once the pin has been quiet for this long.
//
#define BUTTON_DEBOUNCE_MS 20

// How long the button has to be held before the factory reset fires.
//
#define FACTORY_RESET_HOLD_SECONDS 5

// TODO Move this to configuration (Maybe even Matter?), so they can be easily changed.
//
#define THERMISTORNOMINAL 10000
#define TEMPERATURENOMINAL 25
#define BCOEFFICIENT 3977
#define SERIESRESISTOR 10000

// CR2032 operating window. The nRF54L15 itself runs well below 2.0V, but a coin
// cell's usable capacity is gone by then, so that is where 0% sits.
//
constexpr uint16_t kMinOperatingVoltageMv = 2000;
constexpr uint16_t kMaxOperatingVoltageMv = 3000;
constexpr uint16_t kWarningVoltageMv = 2400;
constexpr uint16_t kCriticalVoltageMv = 2200;

// Matter reports BatPercentRemaining in half percent units, so 200 == 100%.
//
constexpr uint8_t kMaxBatteryPercentage = 200;

// The battery channel is deliberately left out of this array. It is owned by
// battery.cpp, which picks it out of zephyr,user by name.
//
static const struct adc_dt_spec adc_channels[] = {
	ADC_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), probe_1),
	ADC_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), probe_2),
};

#define PROBE_1_DIVIDER_POWER_NODE DT_NODELABEL(probe_1_divider_power)
#define PROBE_2_DIVIDER_POWER_NODE DT_NODELABEL(probe_2_divider_power)

#define INDICATOR_LED_NODE DT_ALIAS(indicator_led)
#define RESET_BUTTON_NODE DT_ALIAS(reset_button)

static const struct gpio_dt_spec probe_1_divider_power = GPIO_DT_SPEC_GET(PROBE_1_DIVIDER_POWER_NODE, gpios);
static const struct gpio_dt_spec probe_2_divider_power = GPIO_DT_SPEC_GET(PROBE_2_DIVIDER_POWER_NODE, gpios);

static const struct gpio_dt_spec indicator_led = GPIO_DT_SPEC_GET(INDICATOR_LED_NODE, gpios);
static const struct gpio_dt_spec reset_button = GPIO_DT_SPEC_GET(RESET_BUTTON_NODE, gpios);

constexpr EndpointId kLightEndpointId = 0;
constexpr EndpointId kPowerSourceEndpointId = 0;
constexpr EndpointId kDualTemperatureEndpointId = 3;

Identify sIdentify = {kLightEndpointId, AppTask::IdentifyStartHandler, AppTask::IdentifyStopHandler, Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator};

/// @brief This function is called when an identify start command is received. It will rapidly blink the indicator LED.
/// @param
void AppTask::IdentifyStartHandler(Identify *)
{
	k_timer_stop(&sIndicatorTimer);
	k_timer_start(&sIndicatorTimer, K_MSEC(500), K_MSEC(500));
}

/// @brief This function is called when an identify stop command is received. It will turn off the indicator LED.
/// @param
void AppTask::IdentifyStopHandler(Identify *)
{
	k_timer_stop(&sIndicatorTimer);
	gpio_pin_set_dt(&indicator_led, 0);
}

/// @brief This callback is called when the sensor timer expires. It will post a task to read the temperature sensors.
/// @param timer
void AppTask::SensorTimerCallback(k_timer *timer)
{
	Nrf::PostTask([]
				  { AppTask::SensorMeasureHandler(); });
}

/// @brief This callback is called when the battery timer expires. It will post a task to read the battery voltage.
/// @param timer
void AppTask::BatteryTimerCallback(k_timer *timer)
{
	Nrf::PostTask([]
				  { AppTask::BatteryMeasureHandler(); });
}

void AppTask::IndicatorTimerCallback(k_timer *timer)
{
	// LOG_DBG("LED Indicator: %d", mIndicatorState);

	mIndicatorState = !mIndicatorState;

	gpio_pin_set_dt(&indicator_led, mIndicatorState);
}

void AppTask::FactoryResetTimerCallback(k_timer *timer)
{
	gpio_pin_set_dt(&indicator_led, 0);

	Nrf::PostTask([]
				  { AppTask::FactoryResetHandler(); });
}

/// @brief Runs on the app task, so it is safe to touch the CHIP stack here.
void AppTask::FactoryResetHandler()
{
	LOG_INF("Factory Reset Triggered!");

	chip::Server::GetInstance().ScheduleFactoryReset();
}

#ifdef CONFIG_CHIP_ICD_UAT_SUPPORT
/// @brief Hands the ICD wake-up to the CHIP thread.
///
/// ScheduleWork() rather than LockChipStack() on the app task: the CHIP thread
/// already holds the stack lock when it runs the work, and the app task is left
/// free to keep dispatching - a button release, or a sensor read, is not stuck
/// behind however long the ICD manager takes.
void AppTask::UserActiveModeHandler()
{
	LOG_INF("ICD UserActiveMode is enabled. Moving to ActiveMode...");

	chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t)
												  {
		Server::GetInstance().GetICDManager().OnNetworkActivity();

		LOG_INF("Successfully triggered ICD UserActiveMode!"); },
												  0);
}
#endif

void AppTask::MatterEventHandler(const ChipDeviceEvent *event, intptr_t data)
{
	static bool isNetworkProvisioned = false;
	static bool isBleConnected = false;

	switch (event->Type)
	{
	case DeviceEventType::kCHIPoBLEAdvertisingChange:
		isBleConnected = ConnectivityMgr().NumBLEConnections() != 0;
		break;
	case DeviceEventType::kThreadStateChange:
		isNetworkProvisioned = ConnectivityMgrImpl().IsIPv6NetworkProvisioned() && ConnectivityMgrImpl().IsIPv6NetworkEnabled();
		break;
	default:
		break;
	}

	// The button owns the LED while it is held.
	//
	if (sButtonPressed)
	{
		return;
	}

	if (isNetworkProvisioned)
	{
		LOG_INF("Network is provisioned!");

		k_timer_stop(&sIndicatorTimer);

		gpio_pin_set_dt(&indicator_led, 0);

		// Wait 1s then fire the timer for the first time. Then fire every SENSOR_READ_INTERVAL
		k_timer_start(&sSensorTimer, K_MSEC(1000), K_MSEC(SENSOR_READ_INTERVAL));
	}
	else if (isBleConnected)
	{
		LOG_INF("Bluetooth connection opened");
		k_timer_stop(&sIndicatorTimer);
		k_timer_start(&sIndicatorTimer, K_MSEC(200), K_MSEC(200));
	}
	else if (ConnectivityMgr().IsBLEAdvertising())
	{
		LOG_INF("Bluetooth is advertising");
		k_timer_stop(&sIndicatorTimer);
		k_timer_start(&sIndicatorTimer, K_MSEC(1000), K_MSEC(1000));
	}
	else
	{
		LOG_INF("Bluetooth is disconnected");
		k_timer_stop(&sIndicatorTimer);
		k_timer_start(&sIndicatorTimer, K_MSEC(1000), K_MSEC(1000));
	}
}

#ifdef CONFIG_CHIP_ENABLE_ICD_SUPPORT
/// @brief Logs every ICD idle/active transition.
///
/// The stack keeps this state to itself - nothing in the ICD manager logs a
/// transition - so without an observer there is no way to tell from the console
/// when a long idle period actually began. The callbacks run synchronously on
/// the CHIP thread from inside the state change, so they must stay short.
///
class ICDStateLogger : public ICDStateObserver
{
public:
	void OnEnterIdleMode() override
	{
		ICDConfigurationData &config = ICDConfigurationData::GetInstance();

		LOG_INF("ICD: entered IdleMode for up to %u s, polling every %u ms",
				static_cast<unsigned int>(config.GetIdleModeDuration().count()),
				static_cast<unsigned int>(config.GetSlowPollingInterval().count()));
	}

	void OnEnterActiveMode() override
	{
		ICDConfigurationData &config = ICDConfigurationData::GetInstance();

		LOG_INF("ICD: entered ActiveMode for %u ms, polling every %u ms",
				static_cast<unsigned int>(config.GetActiveModeDuration().count()),
				static_cast<unsigned int>(config.GetFastPollingInterval().count()));
	}

	/// @brief Fires ICD_ACTIVE_TIME_JITTER_MS (300 ms) before IdleMode is entered,
	/// and only once per active period.
	void OnTransitionToIdle() override
	{
		LOG_INF("ICD: about to enter IdleMode");
	}

	void OnICDModeChange() override
	{
		LOG_INF("ICD: operating mode is now %s",
				ICDConfigurationData::GetInstance().GetICDMode() == ICDConfigurationData::ICDMode::LIT ? "LIT" : "SIT");
	}
};

ICDStateLogger sICDStateLogger;
#endif

CHIP_ERROR AppTask::Init()
{
	LOG_INF("Init()");

	ReturnErrorOnFailure(Nrf::Matter::PrepareServer(Nrf::Matter::InitData{.mDeviceInfoProvider = &DeviceInfoProviderImpl::GetDefaultInstance()}));

#ifdef CONFIG_CHIP_ENABLE_ICD_SUPPORT
	// Registered here rather than after StartServer(), because ICDManager::Init()
	// drops straight into IdleMode and an observer added later would miss that
	// first transition. PrepareServer() only queues the server initialisation -
	// the CHIP thread does not run until StartServer() - so there is nothing to
	// race with, and the observer pool itself is alive from static init.
	//
	if (Server::GetInstance().GetICDManager().RegisterObserver(&sICDStateLogger) == nullptr)
	{
		LOG_ERR("Could not register the ICD state logger - CHIP_CONFIG_ICD_OBSERVERS_POOL_SIZE is too small");
	}
#endif

	k_timer_init(&sIndicatorTimer, &IndicatorTimerCallback, nullptr);
	k_timer_user_data_set(&sIndicatorTimer, this);

	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(AppTask::MatterEventHandler, 0));

	// Every timer is initialised before ConfigureGPIO(), because that enables
	// the button interrupt and the callback starts the debounce timer.
	//
	k_timer_init(&sSensorTimer, &SensorTimerCallback, nullptr);
	k_timer_user_data_set(&sSensorTimer, this);

	k_timer_init(&sBatteryTimer, &BatteryTimerCallback, nullptr);
	k_timer_user_data_set(&sBatteryTimer, this);

	k_timer_init(&sFactoryResetTimer, &FactoryResetTimerCallback, nullptr);
	k_timer_user_data_set(&sFactoryResetTimer, this);

	k_timer_init(&sButtonDebounceTimer, &ButtonDebounceTimerCallback, nullptr);
	k_timer_user_data_set(&sButtonDebounceTimer, this);

	ConfigureGPIO();

	// Turn on the indicator LED to start with.
	// Gives an indication that the device is alive!
	//
	gpio_pin_set_dt(&indicator_led, 1);

	k_sleep(K_SECONDS(1));

	gpio_pin_set_dt(&indicator_led, 0);

	return Nrf::Matter::StartServer();
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	// Started here, after StartServer(), so the first measurement cannot land
	// before the PowerSource cluster exists. Unlike the sensor timer this is not
	// held back until the device is provisioned - the battery level should be
	// populated before a controller first reads it during commissioning.
	//
	k_timer_start(&sBatteryTimer, K_MSEC(1000), K_HOURS(BATTERY_READ_INTERVAL_HOURS));

	while (true)
	{
		Nrf::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}

void DeferUserActiveMode(intptr_t arg)
{
#ifdef CONFIG_CHIP_ICD_UAT_SUPPORT
    // Safely notify the ICD Manager of user/network activity to force UserActiveMode
    Server::GetInstance().GetICDManager().OnNetworkActivity();
    
    // Optional: Log the status using the Matter logging system
    LOG_INF("ICD UserActiveMode has been safely triggered via background task.");
#endif
}

void AppTask::ResetButtonCallback(const struct device *dev, struct gpio_callback *cb, gpio_port_pins_t pins)
{
	bool pressed = gpio_pin_get_dt(&reset_button) == 1;

	if(pressed) {
		LOG_INF("Reset Button Pressed");
		chip::DeviceLayer::PlatformMgr().ScheduleWork(DeferUserActiveMode, 0);
	}
	else {
		LOG_INF("Reset Button Released");
	}
	
	// // This runs in interrupt context. It must do as close to nothing as
	// // possible - in particular no logging, because CONFIG_LOG_MODE_IMMEDIATE
	// // writes to the backend inline and doing that from an ISR can deadlock
	// // against a thread that is already inside the logger.
	// //
	// // Every edge just restarts the debounce timer, so the pin is only sampled
	// // once the switch has stopped bouncing.
	// //
	// k_timer_start(&sButtonDebounceTimer, K_MSEC(BUTTON_DEBOUNCE_MS), K_NO_WAIT);
}

/// @brief Fires once the button has been quiet for BUTTON_DEBOUNCE_MS. Still
/// interrupt context, so the real work goes to the app task.
void AppTask::ButtonDebounceTimerCallback(k_timer *timer)
{
	Nrf::PostTask([]
				  { AppTask::ResetButtonHandler(); });
}

/// @brief Runs on the app task, where logging and the CHIP stack lock are safe.
void AppTask::ResetButtonHandler()
{
	bool pressed = gpio_pin_get_dt(&reset_button) == 1;

	// A bounce that settled back where it started is not a state change.
	//
	if (pressed == sButtonPressed)
	{
		return;
	}

	sButtonPressed = pressed;

	if (pressed)
	{
		LOG_INF("Reset Button Pushed");

		// Take the LED off the indicator timer for as long as the button is
		// held, otherwise the blink pattern fights the solid "button down"
		// state - OnNetworkActivity() below generates CHIP events, and
		// MatterEventHandler restarts the indicator timer on every one.
		//
		k_timer_stop(&sIndicatorTimer);

		gpio_pin_set_dt(&indicator_led, 1);

#ifdef CONFIG_CHIP_ICD_UAT_SUPPORT
		UserActiveModeHandler();
#endif

		k_timer_start(&sFactoryResetTimer, K_SECONDS(FACTORY_RESET_HOLD_SECONDS), K_NO_WAIT);
	}
	else
	{
		LOG_INF("Reset Button Released");

		gpio_pin_set_dt(&indicator_led, 0);
		k_timer_stop(&sFactoryResetTimer);
	}
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

	BatteryMeasurementInit();

	if (!gpio_is_ready_dt(&indicator_led))
	{
		LOG_ERR("Cannot configure indicator LED");
		return;
	}

	err = gpio_pin_configure_dt(&indicator_led, GPIO_OUTPUT_INACTIVE);
	if (err != 0)
	{
		LOG_ERR("Configuring indicator pin failed (err: %d)", err);
		return;
	}

	LOG_INF("Successfully configured indicator LED");

	if (!gpio_is_ready_dt(&reset_button))
	{
		LOG_ERR("Reset button is not ready");
		return;
	}

	err = gpio_pin_configure_dt(&reset_button, GPIO_INPUT | GPIO_ACTIVE_HIGH);

	if (err != 0)
	{
		LOG_ERR("Configuring reset button failed (err: %d)", err);
		return;
	}

	// Both edges, so a release before the 5s hold can cancel the factory reset.
	//
	err = gpio_pin_interrupt_configure_dt(&reset_button, GPIO_INT_EDGE_BOTH);

	if (err != 0)
	{
		LOG_ERR("Configuring reset button interrupt failed (err: %d)", err);
		return;
	}

	static struct gpio_callback reset_button_cb_data;

	gpio_init_callback(&reset_button_cb_data, AppTask::ResetButtonCallback, BIT(reset_button.pin));

	err = gpio_add_callback(reset_button.port, &reset_button_cb_data);

	if (err != 0)
	{
		LOG_ERR("Adding callback to reset button failed (err: %d)", err);
		return;
	}

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
		LOG_ERR("Could not initialise ADC%d (%d)", channel, err);
		return -1;
	}

	err = adc_read_dt(&adc_channel, &adc_sequence);

	if (err < 0)
	{
		LOG_ERR("Could not read ADC%d (%d)", channel, err);
		return -1;
	}

	int32_t val_mv = (int32_t)adc_sequence_buf;

	err = adc_raw_to_millivolts_dt(&adc_channel, &val_mv);

	if (err < 0)
	{
		LOG_ERR(" (value in mV not available)\n");
		return -1;
	}

	float resistance = (val_mv * SERIESRESISTOR) / (2200 /* Ref voltage of 900 with a GAIN of 1_2 */ - val_mv);

	double steinhart;
	steinhart = resistance / THERMISTORNOMINAL;		  // (R/Ro)
	steinhart = log(steinhart);						  // ln(R/Ro)
	steinhart /= BCOEFFICIENT;						  // 1/B * ln(R/Ro)
	steinhart += 1.0 / (TEMPERATURENOMINAL + 273.15); // + (1/To)
	steinhart = 1.0 / steinhart;					  // Invert
	steinhart -= 273.15;							  // Convert to Celcius

	double value = steinhart;

	LOG_INF("CHANNEL #%d: V: %" PRId32 " mV", channel, val_mv);
	// LOG_INF("A: %d", adc_sequence);
	// LOG_INF("R: %d", (int)resistance);
	// LOG_INF("T: %d", value);

	return value;
}

void AppTask::SensorMeasureHandler()
{
	// Switch on the power pins. Let the power stay on for a short period of
	// time so the voltage stabalises.
	//
	gpio_pin_set_dt(&probe_1_divider_power, 1);
	k_sleep(K_MSEC(50));

	int16_t probe_1_temperature = read_probe_temperature(1) * 100; // Convert temperature to Matter
	gpio_pin_set_dt(&probe_1_divider_power, 0);

	gpio_pin_set_dt(&probe_2_divider_power, 1);
	k_sleep(K_MSEC(50));

	int16_t probe_2_temperature = read_probe_temperature(2) * 100; // Convert temperature to Matter
	gpio_pin_set_dt(&probe_2_divider_power, 0);

	// With both readings taken, update both of the clusters.
	//
	chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(1, probe_1_temperature);
	chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(2, probe_2_temperature);

	// Publish the same pair to the manufacturer-specific cluster, so a single read
	// returns both probes as sampled in this pass.
	//
	//Clusters::DualTemperatureMeasurement::Attributes::Probe1MeasuredValue::Set(kDualTemperatureEndpointId, probe_1_temperature);
	//Clusters::DualTemperatureMeasurement::Attributes::Probe2MeasuredValue::Set(kDualTemperatureEndpointId, probe_2_temperature);
}

void AppTask::BatteryMeasureHandler()
{
	int32_t voltageMv = BatteryMeasurementReadVoltageMv();

	if (voltageMv < 0)
	{
		LOG_ERR("Battery measurement failed (%" PRId32 ")", voltageMv);

		Clusters::PowerSource::Attributes::Status::Set(kPowerSourceEndpointId, Clusters::PowerSource::PowerSourceStatusEnum::kUnavailable);
		Clusters::PowerSource::Attributes::BatPresent::Set(kPowerSourceEndpointId, false);
		return;
	}

	uint8_t percentage;

	if (voltageMv <= kMinOperatingVoltageMv)
	{
		percentage = 0;
	}
	else if (voltageMv >= kMaxOperatingVoltageMv)
	{
		percentage = kMaxBatteryPercentage;
	}
	else
	{
		percentage = static_cast<uint8_t>(kMaxBatteryPercentage * (voltageMv - kMinOperatingVoltageMv) / (kMaxOperatingVoltageMv - kMinOperatingVoltageMv));
	}

	Clusters::PowerSource::BatChargeLevelEnum chargeLevel;

	if (voltageMv < kCriticalVoltageMv)
	{
		chargeLevel = Clusters::PowerSource::BatChargeLevelEnum::kCritical;
	}
	else if (voltageMv < kWarningVoltageMv)
	{
		chargeLevel = Clusters::PowerSource::BatChargeLevelEnum::kWarning;
	}
	else
	{
		chargeLevel = Clusters::PowerSource::BatChargeLevelEnum::kOk;
	}

	LOG_INF("Battery: %" PRId32 " mV, %u%% (level %u)", voltageMv, percentage / 2, static_cast<uint8_t>(chargeLevel));

	Clusters::PowerSource::Attributes::Status::Set(kPowerSourceEndpointId, Clusters::PowerSource::PowerSourceStatusEnum::kActive);
	Clusters::PowerSource::Attributes::BatPresent::Set(kPowerSourceEndpointId, true);
	Clusters::PowerSource::Attributes::BatVoltage::Set(kPowerSourceEndpointId, static_cast<uint32_t>(voltageMv));
	Clusters::PowerSource::Attributes::BatPercentRemaining::Set(kPowerSourceEndpointId, percentage);
	Clusters::PowerSource::Attributes::BatChargeLevel::Set(kPowerSourceEndpointId, chargeLevel);
}

/// @brief Customises the PowerSource cluster.
/// @param endpoint
void emberAfPowerSourceClusterInitCallback(chip::EndpointId endpoint)
{
	LOG_INF("emberAfPowerSourceClusterInitCallback()");

	Clusters::PowerSource::Attributes::Status::Set(endpoint, Clusters::PowerSource::PowerSourceStatusEnum::kActive);

	Clusters::PowerSource::Attributes::Order::Set(endpoint, 0);

	Clusters::PowerSource::Attributes::Description::Set(endpoint, chip::CharSpan::fromCharString("Battery"));

	Clusters::PowerSource::Attributes::BatReplaceability::Set(endpoint, Clusters::PowerSource::BatReplaceabilityEnum::kUserReplaceable);

	Clusters::PowerSource::Attributes::BatReplacementNeeded::Set(endpoint, false);
}

/// @brief Required by MATTER_PLUGINS_INIT. The DualTemperatureMeasurement cluster is
/// manufacturer-specific, so the Matter SDK provides no implementation of this hook.
/// Nothing needs initialising: both attributes are RAM backed and are written by
/// SensorMeasureHandler().
void MatterDualTemperatureMeasurementPluginServerInitCallback()
{
}
