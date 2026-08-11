/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"

#include "app/matter_init.h"
#include "app/task_executor.h"
#include "lib/core/CHIPError.h"
#include "lib/support/CodeUtils.h"

#include <setup_payload/OnboardingCodesUtil.h>

#include <zephyr/logging/log.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

#include "battery.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/callback.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/clusters/power-source-server/power-source-server.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace
{
k_timer sIndicatorTimer;
k_timer sBatteryTimer;
k_timer sFactoryResetTimer;
bool sIndicatorState;

bool sIsNetworkProvisioned;
bool sIsBleConnected;

#define BATTERY_READ_INTERVAL 5000
#define FACTORY_RESET_HOLD_SECONDS 10

/* CR2032 operating window. The nRF54L15 itself runs well below 2.0 V, but a
 * coin cell's usable capacity is gone by then, so that is where 0% sits.
 */
constexpr uint16_t kMinOperatingVoltageMv = 2000;
constexpr uint16_t kMaxOperatingVoltageMv = 3000;
constexpr uint16_t kWarningVoltageMv = 2400;
constexpr uint16_t kCriticalVoltageMv = 2200;

/* Matter reports BatPercentRemaining in half-percent units, so 200 == 100%. */
constexpr uint8_t kMaxBatteryPercentage = 200;

constexpr EndpointId kPowerSourceEndpointId = 0;

#define INDICATOR_LED_NODE DT_ALIAS(indicator_led)
#define RESET_BUTTON_NODE DT_ALIAS(reset_button)

const struct gpio_dt_spec sIndicatorLed = GPIO_DT_SPEC_GET(INDICATOR_LED_NODE, gpios);
const struct gpio_dt_spec sResetButton = GPIO_DT_SPEC_GET(RESET_BUTTON_NODE, gpios);

Identify sIdentify = {kPowerSourceEndpointId, AppTask::IdentifyStartHandler,
		      AppTask::IdentifyStopHandler,
		      Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator};
} /* namespace */

/// @brief This function is called when an identify start command is received. It will rapidly blink the indicator LED.
/// @param
void AppTask::IdentifyStartHandler(Identify *)
{
	k_timer_stop(&sIndicatorTimer);
	k_timer_start(&sIndicatorTimer, K_MSEC(500), K_MSEC(500));
}

/// @brief This function is called when an identify stop command is received. It will restore the connectivity indication.
/// @param
void AppTask::IdentifyStopHandler(Identify *)
{
	UpdateIndicatorLed();
}

/// @brief This callback is called when the battery timer expires. It will post a task to read the battery voltage.
/// @param timer
void AppTask::BatteryTimerCallback(k_timer *timer)
{
	Nrf::PostTask([] { AppTask::BatteryMeasureHandler(); });
}

void AppTask::IndicatorTimerCallback(k_timer *timer)
{
	sIndicatorState = !sIndicatorState;

	gpio_pin_set_dt(&sIndicatorLed, sIndicatorState);
}

void AppTask::FactoryResetTimerCallback(k_timer *timer)
{
	k_timer_stop(&sIndicatorTimer);
	gpio_pin_set_dt(&sIndicatorLed, 0);

	/* Timer callbacks run in interrupt context, so hand the reset itself to
	 * the app task.
	 */
	Nrf::PostTask([] {
		LOG_INF("Factory Reset Triggered!");
		chip::Server::GetInstance().ScheduleFactoryReset();
	});
}

void AppTask::UpdateIndicatorLed()
{
	k_timer_stop(&sIndicatorTimer);

	if (sIsNetworkProvisioned) {
		/* Provisioned and idle - the LED stays dark so the battery
		 * reading is not skewed by the LED current.
		 */
		gpio_pin_set_dt(&sIndicatorLed, 0);
	} else if (sIsBleConnected) {
		k_timer_start(&sIndicatorTimer, K_MSEC(200), K_MSEC(200));
	} else {
		k_timer_start(&sIndicatorTimer, K_MSEC(1000), K_MSEC(1000));
	}
}

void AppTask::MatterEventHandler(const ChipDeviceEvent *event, intptr_t data)
{
	switch (event->Type) {
	case DeviceEventType::kCHIPoBLEAdvertisingChange:
		sIsBleConnected = ConnectivityMgr().NumBLEConnections() != 0;
		break;
	case DeviceEventType::kThreadStateChange:
		sIsNetworkProvisioned = ConnectivityMgrImpl().IsIPv6NetworkProvisioned() &&
					ConnectivityMgrImpl().IsIPv6NetworkEnabled();
		break;
	default:
		break;
	}

	if (sIsNetworkProvisioned) {
		LOG_INF("Network is provisioned!");
	} else if (sIsBleConnected) {
		LOG_INF("Bluetooth connection opened");
	} else if (ConnectivityMgr().IsBLEAdvertising()) {
		LOG_INF("Bluetooth is advertising");
	} else {
		LOG_INF("Bluetooth is disconnected");
	}

	UpdateIndicatorLed();
}

CHIP_ERROR AppTask::Init()
{
	LOG_INF("Init()");

	ReturnErrorOnFailure(Nrf::Matter::PrepareServer());

	k_timer_init(&sIndicatorTimer, &IndicatorTimerCallback, nullptr);
	k_timer_user_data_set(&sIndicatorTimer, this);

	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(AppTask::MatterEventHandler, 0));

	ConfigureGPIO();

	if (BatteryMeasurementInit() < 0) {
		return CHIP_ERROR_INCORRECT_STATE;
	}

	/* Turn on the indicator LED to start with.
	 * Gives an indication that the device is alive!
	 */
	gpio_pin_set_dt(&sIndicatorLed, 1);

	k_sleep(K_SECONDS(1));

	gpio_pin_set_dt(&sIndicatorLed, 0);

	k_timer_init(&sBatteryTimer, &BatteryTimerCallback, nullptr);
	k_timer_user_data_set(&sBatteryTimer, this);

	k_timer_init(&sFactoryResetTimer, &FactoryResetTimerCallback, nullptr);
	k_timer_user_data_set(&sFactoryResetTimer, this);

	return Nrf::Matter::StartServer();
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	/* Started here rather than in Init() so the first measurement cannot land
	 * before StartServer() has finished bringing the cluster up. Measuring
	 * from boot rather than from provisioning is what makes this useful on
	 * the bench - the readings do not wait for a controller.
	 */
	k_timer_start(&sBatteryTimer, K_MSEC(1000), K_MSEC(BATTERY_READ_INTERVAL));

	while (true) {
		Nrf::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}

void AppTask::ResetButtonCallback(const struct device *dev, struct gpio_callback *cb,
				  gpio_port_pins_t pins)
{
	/* The pin is configured from the devicetree flags alone, so a logical 1
	 * means pressed on both the active-high custom board and the active-low DK.
	 */
	if (gpio_pin_get_dt(&sResetButton) == 1) {
		LOG_INF("Reset Button Pushed");

		k_timer_stop(&sIndicatorTimer);
		gpio_pin_set_dt(&sIndicatorLed, 1);

#ifdef CONFIG_CHIP_ICD_UAT_SUPPORT
		/* This runs in the GPIO ISR, and taking the CHIP stack lock from
		 * an ISR is not allowed, so hand the notification to the app task.
		 */
		Nrf::PostTask([] {
			LOG_INF("ICD UserActiveMode is enabled. Moving to ActiveMode...");

			chip::DeviceLayer::PlatformMgr().LockChipStack();
			Server::GetInstance().GetICDManager().OnNetworkActivity();
			chip::DeviceLayer::PlatformMgr().UnlockChipStack();

			LOG_INF("Successfully triggered ICD UserActiveMode!");
		});
#endif

		k_timer_start(&sFactoryResetTimer, K_SECONDS(FACTORY_RESET_HOLD_SECONDS), K_NO_WAIT);
	} else {
		LOG_INF("Reset Button Released");

		k_timer_stop(&sFactoryResetTimer);
		UpdateIndicatorLed();
	}
}

void AppTask::ConfigureGPIO()
{
	if (!gpio_is_ready_dt(&sIndicatorLed)) {
		LOG_ERR("Cannot configure indicator LED");
		return;
	}

	int err = gpio_pin_configure_dt(&sIndicatorLed, GPIO_OUTPUT_INACTIVE);
	if (err != 0) {
		LOG_ERR("Configuring indicator pin failed (err: %d)", err);
		return;
	}

	LOG_INF("Successfully configured indicator LED");

	if (!gpio_is_ready_dt(&sResetButton)) {
		LOG_ERR("Reset button is not ready");
		return;
	}

	/* No polarity flags here - the pull and active level come from the
	 * devicetree, which differs between the DK and the custom board.
	 */
	err = gpio_pin_configure_dt(&sResetButton, GPIO_INPUT);

	if (err != 0) {
		LOG_ERR("Configuring reset button failed (err: %d)", err);
		return;
	}

	/* Both edges, so that the release is seen and the factory reset timer
	 * can be cancelled.
	 */
	err = gpio_pin_interrupt_configure_dt(&sResetButton, GPIO_INT_EDGE_BOTH);

	if (err != 0) {
		LOG_ERR("Configuring reset button interrupt failed (err: %d)", err);
		return;
	}

	static struct gpio_callback reset_button_cb_data;

	gpio_init_callback(&reset_button_cb_data, AppTask::ResetButtonCallback, BIT(sResetButton.pin));

	err = gpio_add_callback(sResetButton.port, &reset_button_cb_data);

	if (err != 0) {
		LOG_ERR("Adding callback to reset button failed (err: %d)", err);
		return;
	}

	LOG_INF("Successfully configured reset button");
}

void AppTask::BatteryMeasureHandler()
{
	int32_t voltageMv = BatteryMeasurementReadVoltageMv();

	if (voltageMv < 0) {
		LOG_ERR("Battery measurement failed (%" PRId32 ")", voltageMv);

		Clusters::PowerSource::Attributes::Status::Set(
			kPowerSourceEndpointId,
			Clusters::PowerSource::PowerSourceStatusEnum::kUnavailable);
		Clusters::PowerSource::Attributes::BatPresent::Set(kPowerSourceEndpointId, false);
		return;
	}

	uint8_t percentage;

	if (voltageMv <= kMinOperatingVoltageMv) {
		percentage = 0;
	} else if (voltageMv >= kMaxOperatingVoltageMv) {
		percentage = kMaxBatteryPercentage;
	} else {
		percentage = static_cast<uint8_t>(kMaxBatteryPercentage *
						  (voltageMv - kMinOperatingVoltageMv) /
						  (kMaxOperatingVoltageMv - kMinOperatingVoltageMv));
	}

	Clusters::PowerSource::BatChargeLevelEnum chargeLevel;

	if (voltageMv < kCriticalVoltageMv) {
		chargeLevel = Clusters::PowerSource::BatChargeLevelEnum::kCritical;
	} else if (voltageMv < kWarningVoltageMv) {
		chargeLevel = Clusters::PowerSource::BatChargeLevelEnum::kWarning;
	} else {
		chargeLevel = Clusters::PowerSource::BatChargeLevelEnum::kOk;
	}

	LOG_INF("Battery: %" PRId32 " mV, %u%% (level %u)", voltageMv, percentage / 2,
		static_cast<uint8_t>(chargeLevel));

	Protocols::InteractionModel::Status status;

	status = Clusters::PowerSource::Attributes::Status::Set(
		kPowerSourceEndpointId, Clusters::PowerSource::PowerSourceStatusEnum::kActive);
	if (status != Protocols::InteractionModel::Status::Success) {
		LOG_ERR("Updating PowerSource Status failed %x", to_underlying(status));
	}

	status = Clusters::PowerSource::Attributes::BatPresent::Set(kPowerSourceEndpointId, true);
	if (status != Protocols::InteractionModel::Status::Success) {
		LOG_ERR("Updating PowerSource BatPresent failed %x", to_underlying(status));
	}

	status = Clusters::PowerSource::Attributes::BatVoltage::Set(kPowerSourceEndpointId,
								   static_cast<uint32_t>(voltageMv));
	if (status != Protocols::InteractionModel::Status::Success) {
		LOG_ERR("Updating PowerSource BatVoltage failed %x", to_underlying(status));
	}

	status = Clusters::PowerSource::Attributes::BatPercentRemaining::Set(kPowerSourceEndpointId,
									    percentage);
	if (status != Protocols::InteractionModel::Status::Success) {
		LOG_ERR("Updating PowerSource BatPercentRemaining failed %x", to_underlying(status));
	}

	status = Clusters::PowerSource::Attributes::BatChargeLevel::Set(kPowerSourceEndpointId,
								       chargeLevel);
	if (status != Protocols::InteractionModel::Status::Success) {
		LOG_ERR("Updating PowerSource BatChargeLevel failed %x", to_underlying(status));
	}
}

/// @brief Customises the PowerSource cluster.
/// @param endpoint
void emberAfPowerSourceClusterInitCallback(chip::EndpointId endpoint)
{
	LOG_INF("emberAfPowerSourceClusterServerInitCallback()");

	Clusters::PowerSource::Attributes::Status::Set(
		endpoint, Clusters::PowerSource::PowerSourceStatusEnum::kActive);

	Clusters::PowerSource::Attributes::Order::Set(endpoint, 0);

	Clusters::PowerSource::Attributes::Description::Set(endpoint,
							   chip::CharSpan::fromCharString("Battery"));

	Clusters::PowerSource::Attributes::BatChargeLevel::Set(
		endpoint, Clusters::PowerSource::BatChargeLevelEnum::kOk);
	Clusters::PowerSource::Attributes::BatReplacementNeeded::Set(endpoint, false);
	Clusters::PowerSource::Attributes::BatReplaceability::Set(
		endpoint, Clusters::PowerSource::BatReplaceabilityEnum::kUserReplaceable);
}
