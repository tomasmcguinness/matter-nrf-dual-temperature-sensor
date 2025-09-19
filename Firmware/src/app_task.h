/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <platform/CHIPDeviceLayer.h>

struct k_timer;

class AppTask {
public:
	static AppTask &Instance()
	{
		static AppTask sAppTask;
		return sAppTask;
	};

	CHIP_ERROR StartApp();
	static void SensorMeasureHandler();

private:
	CHIP_ERROR Init();
	void ConfigureGPIO();
};
