/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <stdint.h>

/// @brief Sets up the ADC channel used to measure the supply voltage.
/// @return 0 on success, a negative errno otherwise.
int BatteryMeasurementInit();

/// @brief Reads the supply voltage.
///
/// The coin cell drives VDD directly, so the SAADC internal VDD input reads the
/// cell voltage and no divider scaling is applied.
///
/// @return the voltage in millivolts, or a negative errno on failure.
int32_t BatteryMeasurementReadVoltageMv();
