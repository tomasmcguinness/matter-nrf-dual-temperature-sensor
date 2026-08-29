/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 *    @file
 *          Example project configuration file for CHIP.
 *
 *          This is a place to put application or project-specific overrides
 *          to the default configuration values for general CHIP features.
 *
 */

#pragma once

// The nrfconnect platform default is 3, and the stack itself takes all three
// (the report scheduler, the DNS-SD server and the ICD management cluster).
// The application's own ICD state observer needs a fourth slot, otherwise
// RegisterObserver() silently returns nullptr.
//
#define CHIP_CONFIG_ICD_OBSERVERS_POOL_SIZE 4
