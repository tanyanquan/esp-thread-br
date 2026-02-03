/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Combined OpenThread Border Router + BLE Peripheral Example
 *
 * This example demonstrates running a Thread Border Router alongside
 * a BLE peripheral using esp-hosted for communication with an RCP
 * that supports both ot-rcp and esp-hosted (WiFi+BLE).
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"
#include "esp_ot_ota_commands.h"
#include "esp_ot_wifi_cmd.h"
#include "esp_spiffs.h"
#include "esp_vfs_eventfd.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "border_router_launch.h"
#include "esp_br_web.h"

/* ESP-Hosted includes for BLE */
#include "esp_hosted.h"

/* NimBLE includes */
#include "bleprph.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#define TAG "esp_ot_br_ble"

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

/* BLE Configuration */
#if CONFIG_EXAMPLE_RANDOM_ADDR
static uint8_t own_addr_type = BLE_OWN_ADDR_RANDOM;
#else
static uint8_t own_addr_type;
#endif

void ble_store_config_init(void);

static esp_err_t init_spiffs(void)
{
#if CONFIG_AUTO_UPDATE_RCP
    esp_vfs_spiffs_conf_t rcp_fw_conf = {.base_path = "/" CONFIG_RCP_PARTITION_NAME,
                                         .partition_label = CONFIG_RCP_PARTITION_NAME,
                                         .max_files = 10,
                                         .format_if_mount_failed = false};
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&rcp_fw_conf), TAG, "Failed to mount rcp firmware storage");
#endif
#if CONFIG_OPENTHREAD_BR_START_WEB
    esp_vfs_spiffs_conf_t web_server_conf = {
        .base_path = "/spiffs", .partition_label = "web_storage", .max_files = 10, .format_if_mount_failed = false};
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&web_server_conf), TAG, "Failed to mount web storage");
#endif
    return ESP_OK;
}

/**
 * Logs information about a BLE connection to the console.
 */
static void bleprph_print_conn_desc(struct ble_gap_conn_desc *desc)
{
    ESP_LOGI(TAG, "BLE conn handle=%d our_ota_addr_type=%d", desc->conn_handle, desc->our_ota_addr.type);
    ESP_LOGI(TAG, "BLE conn peer_ota_addr_type=%d conn_itvl=%d conn_latency=%d", desc->peer_ota_addr.type,
             desc->conn_itvl, desc->conn_latency);
    ESP_LOGI(TAG, "BLE conn supervision_timeout=%d encrypted=%d authenticated=%d bonded=%d", desc->supervision_timeout,
             desc->sec_state.encrypted, desc->sec_state.authenticated, desc->sec_state.bonded);
}

/**
 * Enables BLE advertising with the following parameters:
 *     o General discoverable mode.
 *     o Undirected connectable mode.
 */
static void bleprph_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    const char *name;
    int rc;

    memset(&fields, 0, sizeof fields);

    /* Advertise two flags:
     *     o Discoverability in forthcoming advertisement (general)
     *     o BLE-only (BR/EDR unsupported).
     */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    /* Indicate that the TX power level field should be included */
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    fields.uuids16 = (ble_uuid16_t[]){BLE_UUID16_INIT(GATT_SVR_SVC_ALERT_UUID)};
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error setting BLE advertisement data; rc=%d", rc);
        return;
    }

    /* Begin advertising */
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, bleprph_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error enabling BLE advertisement; rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "BLE advertising started");
}

/**
 * The nimble host executes this callback when a GAP event occurs.
 */
int bleprph_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
#if defined(BLE_GAP_EVENT_LINK_ESTAB)
    case BLE_GAP_EVENT_LINK_ESTAB:
#else
    case BLE_GAP_EVENT_CONNECT:
#endif
        /* A new connection was established or a connection attempt failed */
        ESP_LOGI(TAG, "BLE connection %s; status=%d", event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);
        if (event->connect.status == 0) {
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc == 0) {
                bleprph_print_conn_desc(&desc);
            }
        } else {
            /* Connection failed; resume advertising */
            bleprph_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnect; reason=%d", event->disconnect.reason);
        bleprph_print_conn_desc(&event->disconnect.conn);
        /* Connection terminated; resume advertising */
        bleprph_advertise();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        /* The central has updated the connection parameters */
        ESP_LOGI(TAG, "BLE connection updated; status=%d", event->conn_update.status);
        rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        if (rc == 0) {
            bleprph_print_conn_desc(&desc);
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "BLE advertise complete; reason=%d", event->adv_complete.reason);
        bleprph_advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        /* Encryption has been enabled or disabled for this connection */
        ESP_LOGI(TAG, "BLE encryption change event; status=%d", event->enc_change.status);
        rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        if (rc == 0) {
            bleprph_print_conn_desc(&desc);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        ESP_LOGD(TAG, "BLE notify_tx event; conn_handle=%d attr_handle=%d status=%d is_indication=%d",
                 event->notify_tx.conn_handle, event->notify_tx.attr_handle, event->notify_tx.status,
                 event->notify_tx.indication);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "BLE subscribe event; conn_handle=%d attr_handle=%d reason=%d", event->subscribe.conn_handle,
                 event->subscribe.attr_handle, event->subscribe.reason);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE mtu update event; conn_handle=%d cid=%d mtu=%d", event->mtu.conn_handle,
                 event->mtu.channel_id, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* We already have a bond with the peer, but it is attempting to
         * establish a new secure link. Delete the old bond and accept new.
         */
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGI(TAG, "BLE PASSKEY_ACTION_EVENT started");
        struct ble_sm_io pkey = {0};

        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.action = event->passkey.params.action;
            pkey.passkey = 123456; // This is the passkey to be entered on peer
            ESP_LOGI(TAG, "Enter passkey %" PRIu32 " on the peer side", pkey.passkey);
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "ble_sm_inject_io result: %d", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            ESP_LOGI(TAG, "Passkey on device's display: %" PRIu32, event->passkey.params.numcmp);
            ESP_LOGI(TAG, "Accept or reject via console: key Y or key N");
            pkey.action = event->passkey.params.action;
            pkey.numcmp_accept = 1; // Auto-accept for this example
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "ble_sm_inject_io result: %d", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
            ESP_LOGI(TAG, "Enter the passkey through console: key 123456");
            pkey.action = event->passkey.params.action;
            pkey.passkey = 123456; // Default passkey
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "ble_sm_inject_io result: %d", rc);
        }
        return 0;
    }

    return 0;
}

static void bleprph_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host resetting state; reason=%d", reason);
}

#if CONFIG_EXAMPLE_RANDOM_ADDR
static void ble_app_set_addr(void)
{
    ble_addr_t addr;
    int rc;

    /* Generate new non-resolvable private address */
    rc = ble_hs_id_gen_rnd(0, &addr);
    assert(rc == 0);

    /* Set generated address */
    rc = ble_hs_id_set_rnd(addr.val);
    assert(rc == 0);
}
#endif

static void bleprph_on_sync(void)
{
    int rc;

#if CONFIG_EXAMPLE_RANDOM_ADDR
    /* Generate a non-resolvable private address */
    ble_app_set_addr();
#endif

    /* Make sure we have proper identity address set (public preferred) */
#if CONFIG_EXAMPLE_RANDOM_ADDR
    rc = ble_hs_util_ensure_addr(1);
#else
    rc = ble_hs_util_ensure_addr(0);
#endif
    assert(rc == 0);

    /* Figure out address to use while advertising (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error determining BLE address type; rc=%d", rc);
        return;
    }

    /* Printing ADDR */
    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);
    ESP_LOGI(TAG, "BLE Device Address: %02x:%02x:%02x:%02x:%02x:%02x", addr_val[5], addr_val[4], addr_val[3],
             addr_val[2], addr_val[1], addr_val[0]);

    /* Begin advertising */
    bleprph_advertise();
}

static void bleprph_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/**
 * Initialize ESP-Hosted and BLE stack
 */
static esp_err_t init_ble_peripheral(void)
{
    esp_err_t ret;
    int rc;

    ESP_LOGI(TAG, "Initializing ESP-Hosted connection to RCP...");

    /* Connect to ESP-Hosted co-processor (RCP) */
    ret = esp_hosted_connect_to_slave();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to ESP-Hosted slave: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Get firmware version from co-processor */
    esp_hosted_coprocessor_fwver_t fwver;
    if (ESP_OK == esp_hosted_get_coprocessor_fwversion(&fwver)) {
        ESP_LOGI(TAG, "ESP-Hosted RCP FW Version: %" PRIu32 ".%" PRIu32 ".%" PRIu32, fwver.major1, fwver.minor1,
                 fwver.patch1);
    } else {
        ESP_LOGW(TAG, "Failed to get ESP-Hosted RCP firmware version");
    }

    /* Initialize BT controller on co-processor */
    ret = esp_hosted_bt_controller_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init BT controller: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Enable BT controller on co-processor */
    ret = esp_hosted_bt_controller_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable BT controller: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "BT controller initialized and enabled on RCP");

    /* Initialize NimBLE port */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE: %d", ret);
        return ret;
    }

    /* Initialize the NimBLE host configuration */
    ble_hs_cfg.reset_cb = bleprph_on_reset;
    ble_hs_cfg.sync_cb = bleprph_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
#ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
#endif
#ifdef CONFIG_EXAMPLE_MITM
    ble_hs_cfg.sm_mitm = 1;
#endif
#ifdef CONFIG_EXAMPLE_USE_SC
    ble_hs_cfg.sm_sc = 1;
#else
    ble_hs_cfg.sm_sc = 0;
#endif

    /* Initialize GATT server */
    rc = gatt_svr_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to init GATT server; rc=%d", rc);
        return ESP_FAIL;
    }

    /* Set the default device name */
    rc = ble_svc_gap_device_name_set("esp-ot-br-ble");
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set BLE device name; rc=%d", rc);
        return ESP_FAIL;
    }

    /* Initialize BLE store configuration */
    ble_store_config_init();

    /* Start the NimBLE host task */
    nimble_port_freertos_init(bleprph_host_task);

    ESP_LOGI(TAG, "BLE peripheral initialized successfully");
    return ESP_OK;
}

void app_main(void)
{
    // Used eventfds:
    // * netif
    // * task queue
    // * border router
    size_t max_eventfd = 3;

#if CONFIG_OPENTHREAD_RADIO_SPINEL_SPI
    // * SpiSpinelInterface (The Spi Spinel Interface needs an eventfd.)
    max_eventfd++;
#endif
#if CONFIG_OPENTHREAD_RADIO_TREL
    // * TREL reception (The Thread Radio Encapsulation Link needs an eventfd for reception.)
    max_eventfd++;
#endif
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = max_eventfd,
    };

    esp_openthread_config_t openthread_config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config =
            {
                .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
                .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
                .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
            },
    };
    esp_rcp_update_config_t rcp_update_config = ESP_OPENTHREAD_RCP_UPDATE_CONFIG();
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(init_spiffs());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if !CONFIG_OPENTHREAD_BR_AUTO_START && CONFIG_EXAMPLE_CONNECT_ETHERNET
// TODO: Add a mechanism for connecting ETH manually.
#error Currently we do not support a manual way to connect ETH, if you want to use ETH, please enable OPENTHREAD_BR_AUTO_START.
#endif

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("esp-ot-br"));
#if CONFIG_OPENTHREAD_CLI_OTA
    esp_set_ota_server_cert((char *)server_cert_pem_start);
#endif

#if CONFIG_OPENTHREAD_BR_START_WEB
    esp_br_web_start("/spiffs");
#endif

    /* Initialize BLE peripheral via ESP-Hosted
     * This connects to the RCP's esp-hosted interface for BLE
     */
    esp_err_t ble_ret = init_ble_peripheral();
    if (ble_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BLE peripheral: %s", esp_err_to_name(ble_ret));
        ESP_LOGW(TAG, "Continuing without BLE functionality...");
    }

    /* Launch OpenThread Border Router
     * This uses the RCP's ot-rcp interface for Thread
     */
    launch_openthread_border_router(&openthread_config, &rcp_update_config);
}
