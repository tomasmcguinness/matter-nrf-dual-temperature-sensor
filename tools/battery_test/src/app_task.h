/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <platform/CHIPDeviceLayer.h>

#include <zephyr/drivers/gpio.h>

using namespace ::chip::DeviceLayer;

struct Identify;

class AppTask
{
public:
	static AppTask &Instance()
	{
		static AppTask sAppTask;
		return sAppTask;
	};

	CHIP_ERROR StartApp();

	static void IdentifyStartHandler(Identify *);
	static void IdentifyStopHandler(Identify *);

private:
	CHIP_ERROR Init();

	static void MatterEventHandler(const ChipDeviceEvent *event, intptr_t data);
	static void BatteryMeasureHandler();
	static void BatteryTimerCallback(k_timer *timer);
	static void IndicatorTimerCallback(k_timer *timer);
	static void FactoryResetTimerCallback(k_timer *timer);

	/// @brief Drives the indicator LED to match the current connectivity state.
	/// Used both by the Matter event handler and to restore the LED after the
	/// button is released.
	static void UpdateIndicatorLed();

	static void ResetButtonCallback(const struct device *dev, struct gpio_callback *cb,
					gpio_port_pins_t pins);

	void ConfigureGPIO();
};
