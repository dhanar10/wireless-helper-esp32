#include "aa_bt.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "aa_bt_client.h"
#include "aa_handshake.h"
#include "sdkconfig.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_sdp_api.h"
#include "esp_spp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "aa_bt";

/* AA Wireless UUID from reference/sdp_clean.c (big-endian wire order) */
static const uint8_t UUID_AA[16] = {
    0x4d, 0xe1, 0x7a, 0x00, 0x52, 0xcb, 0x11, 0xe6,
    0xbd, 0xf4, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66
};

#define UUID16_HFP_AG  0x111F
#define UUID16_HSP_HS  0x1108

static const esp_spp_sec_t s_sec = ESP_SPP_SEC_AUTHENTICATE;
static const esp_spp_role_t s_role = ESP_SPP_ROLE_SLAVE;

/* listen_handle -> scn ; conn_handle -> scn */
static uint8_t s_listen_scn[64];
static uint8_t s_conn_scn[64];
static uint32_t s_listen_handles[64];
static uint32_t s_conn_handles[64];
static size_t s_listen_count;
static size_t s_conn_count;
static int s_servers_started;
static bool s_sdp_ready;
static bool s_discoverable_set;

char *aa_bt_bda_str(const esp_bd_addr_t bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return str;
}

bool aa_bt_is_ready(void)
{
    return s_servers_started >= 3 && s_sdp_ready;
}

size_t aa_bt_get_bonded(esp_bd_addr_t *out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }
    int num = esp_bt_gap_get_bond_device_num();
    if (num <= 0) {
        return 0;
    }
    if ((size_t)num > max) {
        num = (int)max;
    }
    esp_err_t err = esp_bt_gap_get_bond_device_list(&num, out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get_bond_device_list: %s", esp_err_to_name(err));
        return 0;
    }
    return (size_t)num;
}

static void remember_listen(uint32_t handle, uint8_t scn)
{
    if (s_listen_count >= sizeof(s_listen_handles) / sizeof(s_listen_handles[0])) {
        return;
    }
    s_listen_handles[s_listen_count] = handle;
    s_listen_scn[s_listen_count] = scn;
    s_listen_count++;
}

static bool find_listen_scn(uint32_t handle, uint8_t *scn_out, size_t *idx_out)
{
    for (size_t i = 0; i < s_listen_count; i++) {
        if (s_listen_handles[i] == handle) {
            *scn_out = s_listen_scn[i];
            if (idx_out) {
                *idx_out = i;
            }
            return true;
        }
    }
    return false;
}

static void update_listen_handle(size_t idx, uint32_t new_handle)
{
    s_listen_handles[idx] = new_handle;
}

static void remember_conn(uint32_t handle, uint8_t scn)
{
    if (s_conn_count >= sizeof(s_conn_handles) / sizeof(s_conn_handles[0])) {
        return;
    }
    s_conn_handles[s_conn_count] = handle;
    s_conn_scn[s_conn_count] = scn;
    s_conn_count++;
}

static bool find_conn_scn(uint32_t handle, uint8_t *scn_out)
{
    for (size_t i = 0; i < s_conn_count; i++) {
        if (s_conn_handles[i] == handle) {
            *scn_out = s_conn_scn[i];
            return true;
        }
    }
    return false;
}

static void forget_conn(uint32_t handle)
{
    for (size_t i = 0; i < s_conn_count; i++) {
        if (s_conn_handles[i] == handle) {
            s_conn_handles[i] = s_conn_handles[s_conn_count - 1];
            s_conn_scn[i] = s_conn_scn[s_conn_count - 1];
            s_conn_count--;
            return;
        }
    }
}

static void set_uuid128(esp_bt_uuid_t *uuid, const uint8_t *u128)
{
    uuid->len = ESP_UUID_LEN_128;
    memcpy(uuid->uuid.uuid128, u128, ESP_UUID_LEN_128);
}

static void set_uuid16(esp_bt_uuid_t *uuid, uint16_t u16)
{
    uuid->len = ESP_UUID_LEN_16;
    uuid->uuid.uuid16 = u16;
}

static esp_err_t create_raw_sdp(const char *name, int32_t scn, esp_bt_uuid_t *uuid)
{
    /* Match ESP-IDF bt_l2cap_server: use union hdr overlay */
    esp_bluetooth_sdp_record_t record;
    memset(&record, 0, sizeof(record));
    record.hdr.type = ESP_SDP_TYPE_RAW;
    record.hdr.uuid = *uuid;
    record.hdr.service_name_length = (uint32_t)strlen(name) + 1;
    record.hdr.service_name = (char *)name;
    record.hdr.rfcomm_channel_number = scn;
    record.hdr.l2cap_psm = -1;
    record.hdr.profile_version = 0x0100;
    return esp_sdp_create_record(&record);
}

static void register_sdp_records(void)
{
    esp_bt_uuid_t uuid;
    esp_err_t err;

    set_uuid128(&uuid, UUID_AA);
    err = create_raw_sdp("Android Auto Wireless", AA_SCN_AA, &uuid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SDP AA create failed: %s", esp_err_to_name(err));
    }

    set_uuid16(&uuid, UUID16_HFP_AG);
    err = create_raw_sdp("HFP AG", AA_SCN_HFP, &uuid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SDP HFP create failed: %s", esp_err_to_name(err));
    }

    set_uuid16(&uuid, UUID16_HSP_HS);
    err = create_raw_sdp("HSP HS", AA_SCN_HSP, &uuid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SDP HSP create failed: %s", esp_err_to_name(err));
    }
}

static void maybe_go_discoverable(void)
{
    if (s_discoverable_set) {
        return;
    }
    if (s_servers_started < 3 || !s_sdp_ready) {
        return;
    }
    esp_bt_gap_set_device_name(CONFIG_AA_BT_DEVICE_NAME);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    s_discoverable_set = true;
    ESP_LOGI(TAG, "Discoverable as '%s'", CONFIG_AA_BT_DEVICE_NAME);
}

static void start_next_server(void)
{
    /* Start servers sequentially after each START_EVT to avoid races */
    if (s_servers_started == 0) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_spp_start_srv(s_sec, s_role, AA_SCN_HSP, "HSP HS"));
    } else if (s_servers_started == 1) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_spp_start_srv(s_sec, s_role, AA_SCN_AA, "AA RFCOMM"));
    } else if (s_servers_started == 2) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_spp_start_srv(s_sec, s_role, AA_SCN_HFP, "HFP AG"));
    }
}

static void on_server_open(uint32_t handle, uint8_t scn, const esp_bd_addr_t rem_bda)
{
    char bda_str[18];
    ESP_LOGI(TAG, "Connection on SCN %u from [%s]", (unsigned)scn,
             aa_bt_bda_str(rem_bda, bda_str, sizeof(bda_str)));
    if (scn == AA_SCN_AA) {
        aa_handshake_on_open(handle);
    }
}

static void esp_sdp_cb(esp_sdp_cb_event_t event, esp_sdp_cb_param_t *param)
{
    switch (event) {
    case ESP_SDP_INIT_EVT:
        ESP_LOGI(TAG, "ESP_SDP_INIT_EVT status=%d", param->init.status);
        if (param->init.status == ESP_SDP_SUCCESS) {
            register_sdp_records();
        }
        break;
    case ESP_SDP_CREATE_RECORD_COMP_EVT:
        ESP_LOGI(TAG, "ESP_SDP_CREATE_RECORD_COMP_EVT status=%d handle=%d",
                 param->create_record.status, param->create_record.record_handle);
        s_sdp_ready = true;
        maybe_go_discoverable();
        break;
    case ESP_SDP_SEARCH_COMP_EVT:
        aa_bt_client_on_sdp_search(param);
        break;
    default:
        break;
    }
}

static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    char bda_str[18] = {0};

    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS) {
            ESP_LOGI(TAG, "ESP_SPP_INIT_EVT");
            aa_bt_client_init();
            if (esp_sdp_register_callback(esp_sdp_cb) == ESP_OK) {
                esp_sdp_init();
            }
            start_next_server();
        } else {
            ESP_LOGE(TAG, "ESP_SPP_INIT_EVT failed status=%d", param->init.status);
        }
        break;

    case ESP_SPP_START_EVT:
        if (param->start.status == ESP_SPP_SUCCESS) {
            ESP_LOGI(TAG, "ESP_SPP_START_EVT handle=%" PRIu32 " scn=%u",
                     param->start.handle, (unsigned)param->start.scn);
            remember_listen(param->start.handle, param->start.scn);
            s_servers_started++;
            if (s_servers_started < 3) {
                start_next_server();
            } else {
                maybe_go_discoverable();
            }
        } else {
            ESP_LOGE(TAG, "ESP_SPP_START_EVT failed status=%d", param->start.status);
        }
        break;

    case ESP_SPP_SRV_OPEN_EVT: {
        ESP_LOGI(TAG, "ESP_SPP_SRV_OPEN_EVT status=%d handle=%" PRIu32
                 " new_listen=%" PRIu32 " rem=[%s]",
                 param->srv_open.status, param->srv_open.handle,
                 param->srv_open.new_listen_handle,
                 aa_bt_bda_str(param->srv_open.rem_bda, bda_str, sizeof(bda_str)));

        /*
         * On accept, the previous listen handle becomes the connection handle,
         * and new_listen_handle continues listening on the same SCN.
         */
        uint8_t scn = 0;
        size_t idx = 0;
        if (find_listen_scn(param->srv_open.handle, &scn, &idx)) {
            update_listen_handle(idx, param->srv_open.new_listen_handle);
            remember_conn(param->srv_open.handle, scn);
            on_server_open(param->srv_open.handle, scn, param->srv_open.rem_bda);
        } else {
            ESP_LOGW(TAG, "SRV_OPEN: unknown listen handle; trying new_listen map");
            if (find_listen_scn(param->srv_open.new_listen_handle, &scn, &idx)) {
                remember_conn(param->srv_open.handle, scn);
                on_server_open(param->srv_open.handle, scn, param->srv_open.rem_bda);
            }
        }
        break;
    }

    case ESP_SPP_CL_INIT_EVT:
        aa_bt_client_on_spp_cl_init(param);
        break;

    case ESP_SPP_OPEN_EVT:
        aa_bt_client_on_spp_open(param);
        break;

    case ESP_SPP_DATA_IND_EVT: {
        uint8_t scn = 0;
        if (!find_conn_scn(param->data_ind.handle, &scn)) {
            /* Outbound poke socket — ignore AT noise */
            break;
        }
        if (scn == AA_SCN_AA) {
            aa_handshake_on_data(param->data_ind.handle, param->data_ind.data,
                                 param->data_ind.len);
        } else if (scn == AA_SCN_HFP) {
            aa_hfp_on_data(param->data_ind.handle, param->data_ind.data,
                           param->data_ind.len);
        }
        break;
    }

    case ESP_SPP_CLOSE_EVT: {
        uint8_t scn = 0;
        bool known = find_conn_scn(param->close.handle, &scn);
        if (known && scn == AA_SCN_AA) {
            aa_handshake_on_close(param->close.handle);
        }
        forget_conn(param->close.handle);
        aa_bt_client_on_spp_close(param);
        ESP_LOGI(TAG, "ESP_SPP_CLOSE_EVT handle=%" PRIu32, param->close.handle);
        break;
    }

    case ESP_SPP_WRITE_EVT:
        if (param->write.status != ESP_SPP_SUCCESS) {
            ESP_LOGW(TAG, "ESP_SPP_WRITE_EVT status=%d", param->write.status);
        }
        break;

    default:
        break;
    }
}

static void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    char bda_str[18] = {0};

    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "auth success: %s [%s]", param->auth_cmpl.device_name,
                     aa_bt_bda_str(param->auth_cmpl.bda, bda_str, sizeof(bda_str)));
        } else {
            ESP_LOGE(TAG, "auth failed status=%d", param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_PIN_REQ_EVT:
        ESP_LOGI(TAG, "PIN_REQ min_16=%d — auto reply 0000", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            esp_bt_pin_code_t pin = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin);
        } else {
            esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        }
        break;

    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "SSP confirm %" PRIu32 " — auto accept", param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey notify: %" PRIu32, param->key_notif.passkey);
        break;

    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "SSP passkey request — not supported, ignore");
        break;

    default:
        break;
    }
}

esp_err_t aa_bt_start(void)
{
    esp_err_t ret;

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "controller init: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "controller enable: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    bluedroid_cfg.ssp_en = true;
    ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid init: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid enable: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(esp_bt_gap_cb));
    ESP_ERROR_CHECK(esp_spp_register_callback(esp_spp_cb));

    esp_spp_cfg_t spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0,
    };
    ret = esp_spp_enhanced_init(&spp_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spp init: %s", esp_err_to_name(ret));
        return ret;
    }

    /* IO capability DisplayYesNo equivalent — auto-confirm in gap cb */
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    char bda_str[18];
    ESP_LOGI(TAG, "Own address:[%s]",
             aa_bt_bda_str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));
    return ESP_OK;
}
