/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "battery.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "No suitable devicetree overlay specified"
#endif

namespace
{
const struct adc_dt_spec sAdc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

uint16_t sAdcBuffer;

struct adc_sequence sAdcSequence = {
	.buffer = &sAdcBuffer,
	.buffer_size = sizeof(sAdcBuffer),
	.calibrate = true,
};
} /* namespace */

int BatteryMeasurementInit()
{
	if (!device_is_ready(sAdc.dev)) {
		LOG_ERR("ADC controller device not ready");
		return -ENODEV;
	}

	int err = adc_channel_setup_dt(&sAdc);

	if (err < 0) {
		LOG_ERR("Could not setup the battery ADC channel (%d)", err);
		return err;
	}

	LOG_INF("Battery measurement initialised");

	return 0;
}

int32_t BatteryMeasurementReadVoltageMv()
{
	int err = adc_sequence_init_dt(&sAdc, &sAdcSequence);

	if (err < 0) {
		LOG_ERR("Could not initialise the battery ADC sequence (%d)", err);
		return err;
	}

	err = adc_read_dt(&sAdc, &sAdcSequence);

	if (err < 0) {
		LOG_ERR("Could not read the battery ADC (%d)", err);
		return err;
	}

	int32_t voltageMv = static_cast<int32_t>(sAdcBuffer);

	err = adc_raw_to_millivolts_dt(&sAdc, &voltageMv);

	if (err < 0) {
		LOG_ERR("Battery value in mV not available (%d)", err);
		return err;
	}

	return voltageMv;
}
