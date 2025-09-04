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

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

k_timer sSensorTimer;

void SensorTimerHandler(k_timer *timer)
{
    Nrf::PostTask([] { AppTask::SensorMeasureHandler(); });
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

 	k_timer_init(&sSensorTimer, &SensorTimerHandler, nullptr);
    k_timer_user_data_set(&sSensorTimer, this);
	k_timer_start(&sSensorTimer, K_MSEC(5000), K_MSEC(5000));

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

void AppTask::SensorMeasureHandler()
{
    chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(1, int16_t(rand() % 5000));
    chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(2, int16_t(rand() % 5000));
}