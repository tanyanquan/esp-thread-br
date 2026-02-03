/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef H_BLEPRPH_
#define H_BLEPRPH_

#include <stdbool.h>
#include "nimble/ble.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ble_hs_cfg;
struct ble_gap_event;
struct ble_gatt_register_ctxt;

/** GATT server UUIDs */
#define GATT_SVR_SVC_ALERT_UUID 0x1811
#define GATT_SVR_CHR_SUP_NEW_ALERT_CAT_UUID 0x2A47
#define GATT_SVR_CHR_NEW_ALERT 0x2A46
#define GATT_SVR_CHR_SUP_UNR_ALERT_CAT_UUID 0x2A48
#define GATT_SVR_CHR_UNR_ALERT_STAT_UUID 0x2A45
#define GATT_SVR_CHR_ALERT_NOT_CTRL_PT 0x2A44

/**
 * @brief GATT server registration callback
 *
 * Called when services/characteristics/descriptors are registered
 */
void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg);

/**
 * @brief Initialize the GATT server
 *
 * @return 0 on success, non-zero on failure
 */
int gatt_svr_init(void);

/**
 * @brief GAP event callback for BLE peripheral
 *
 * @param event The GAP event
 * @param arg User argument (unused)
 * @return 0 on success
 */
int bleprph_gap_event(struct ble_gap_event *event, void *arg);

#ifdef __cplusplus
}
#endif

#endif /* H_BLEPRPH_ */
