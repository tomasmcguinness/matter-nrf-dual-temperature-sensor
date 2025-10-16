/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <platform/CHIPDeviceLayer.h>

using namespace ::chip::DeviceLayer;

class AppTask {
public:
	static AppTask &Instance()
	{
		static AppTask sAppTask;
		return sAppTask;
	};

	CHIP_ERROR StartApp();
	
private:
	CHIP_ERROR Init();

	static void MatterEventHandler(const ChipDeviceEvent *event, intptr_t data);
	static void SensorMeasureHandler();
	static void SensorTimerCallback(k_timer *timer);
	static void IndicatorTimerCallback(k_timer *timer);
	static void FactoryResetTimerCallback(k_timer *timer);

	static void ResetButtonCallback(const struct device *dev, struct gpio_callback *cb, uint32_t pins);

	void ConfigureGPIO();
};