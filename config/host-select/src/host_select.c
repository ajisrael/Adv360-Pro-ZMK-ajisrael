/*
 * Host-driven profile selection: a bonded host writes "select me" to a GATT
 * characteristic and the keyboard makes that host's profile the active one,
 * exactly as if &bt BT_SEL <its profile> had been pressed.
 *
 * Service   98cfc2e6-f4cf-4491-8d9e-4995cc63c032
 * State     a849041c-f1bd-435a-8c17-78a0dea4c480 (read, write; encrypted)
 *   read  -> [version = 1, active profile, caller's profile or 0xff]
 *   write <- [0x01] select the caller's profile
 *
 * Only bonded, encrypted connections can read or write, and a host can only
 * select the profile it is bonded to, never another host's.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/hid.h>

#define HOST_SELECT_VERSION 1
#define HOST_SELECT_CMD_SELECT_ME 0x01
#define HOST_SELECT_NO_PROFILE 0xff

static struct bt_uuid_128 service_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x98cfc2e6, 0xf4cf, 0x4491, 0x8d9e, 0x4995cc63c032));
static struct bt_uuid_128 state_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xa849041c, 0xf1bd, 0x435a, 0x8c17, 0x78a0dea4c480));

/* The latest requested profile (-1 = none) and when it was requested. Written
 * from the Bluetooth thread, consumed on the system work queue, where ZMK's
 * keymap and profile handling run too. */
static atomic_t requested = ATOMIC_INIT(-1);
static atomic_t requested_at;

static void select_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(select_work, select_work_handler);

static bool keys_held(void) {
    const struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    if (report->body.modifiers) {
        return true;
    }
    for (size_t i = 0; i < sizeof(report->body.keys); i++) {
        if (report->body.keys[i]) {
            return true;
        }
    }
    return false;
}

/* Time for the last report to reach the old host before switching: reports
 * are sent from ZMK's HID thread to whichever profile is active by then. */
#define SETTLE_MS 20

/* Whether the keys have already been released for SETTLE_MS. */
static bool settled;

static void select_work_handler(struct k_work *work) {
    int index = atomic_get(&requested);
    if (index < 0) {
        settled = false;
        return;
    }
    if (index == zmk_ble_active_profile_index() && atomic_cas(&requested, index, -1)) {
        /* Already there: nothing to wait for, and no keys to release. */
        settled = false;
        return;
    }
    uint32_t waited = k_uptime_get_32() - (uint32_t)atomic_get(&requested_at);
    bool held = keys_held();
    if (held && waited < CONFIG_ZMK_HOST_SELECT_MAX_DEFER_MS) {
        settled = false;
        k_work_reschedule(&select_work, K_MSEC(10));
        return;
    }
    if (held) {
        /* ZMK's own release on a profile switch goes to the new host; send it
         * to the old one, or the key would stay down there. */
        LOG_WRN("host-select: keys still held after %u ms; releasing them", waited);
        zmk_hid_keyboard_clear();
        zmk_hid_consumer_clear();
        zmk_endpoints_send_report(HID_USAGE_KEY);
        zmk_endpoints_send_report(HID_USAGE_CONSUMER);
        settled = false;
    }
    if (!settled) {
        settled = true;
        k_work_reschedule(&select_work, K_MSEC(SETTLE_MS));
        return;
    }
    settled = false;
    if (!atomic_cas(&requested, index, -1)) {
        /* A newer request arrived meanwhile; handle that one. */
        k_work_reschedule(&select_work, K_NO_WAIT);
        return;
    }
    LOG_INF("host-select: switching to profile %d", index);
    int err = zmk_ble_prof_select(index);
    if (err) {
        LOG_ERR("host-select: selecting profile %d failed (%d)", index, err);
    }
}

static ssize_t read_state(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                          uint16_t len, uint16_t offset) {
    int own = zmk_ble_profile_index(bt_conn_get_dst(conn));
    uint8_t value[] = {HOST_SELECT_VERSION, (uint8_t)zmk_ble_active_profile_index(),
                       own < 0 ? HOST_SELECT_NO_PROFILE : (uint8_t)own};
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static ssize_t write_state(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len != 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    if (((const uint8_t *)buf)[0] != HOST_SELECT_CMD_SELECT_ME) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    int index = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (index < 0) {
        return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    }
    LOG_DBG("host-select: profile %d asks to be selected", index);
    atomic_set(&requested_at, (atomic_val_t)k_uptime_get_32());
    atomic_set(&requested, index);
    /* Unlike reschedule, this leaves a pending wait (held keys, settling) alone. */
    k_work_schedule(&select_work, K_NO_WAIT);
    return len;
}

/* Services get their handles in name order. Sorting last keeps the handles of
 * the existing services (HID in particular) where bonded hosts cached them. */
BT_GATT_SERVICE_DEFINE(zz_host_select_svc, BT_GATT_PRIMARY_SERVICE(&service_uuid),
                       BT_GATT_CHARACTERISTIC(&state_uuid.uuid,
                                              BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                                              BT_GATT_PERM_READ_ENCRYPT |
                                                  BT_GATT_PERM_WRITE_ENCRYPT,
                                              read_state, write_state, NULL));
