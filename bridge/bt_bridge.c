/*
 * BLE (HID over GATT) to USB bridge.
 *
 * The dongle is a Bluetooth central that connects to ONE keyboard and nothing
 * else, and re-emits what the keyboard sends over USB. The keyboard is an
 * ordinary standalone ZMK keyboard: the dongle is just one more entry in its
 * Bluetooth profile list.
 *
 *   keyboard (ZMK, HID over GATT) --BLE--> [this module] --USB HID--> PC
 *
 * What it does:
 *   - scans for the keyboard and connects (bonded address, or the first device
 *     that advertises the HID service with the wanted name when not bonded yet)
 *   - pairs with LE Secure Connections (ZMK requires it) and bonds
 *   - discovers the keyboard's HID service, reads each Report Reference and
 *     subscribes to the input reports
 *   - copies every keyboard / consumer report into ZMK's USB HID
 *   - writes the host's LED state (caps lock...) back to the keyboard
 *
 * Both sides are ZMK built with the same HID options, so the report layouts and
 * ids are identical by construction. A length mismatch is reported and dropped.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/addr.h>

#include <zmk/event_manager.h>
#include <zmk/hid.h>
#include <zmk/usb_hid.h>
#include <zmk/events/hid_indicators_changed.h>

#include "bt_bridge_ad.h"

LOG_MODULE_REGISTER(bt_bridge, CONFIG_ZMK_LOG_LEVEL);

/* ---- HID report Reference (HOGP) ---- */
#define REPORT_TYPE_INPUT  1
#define REPORT_TYPE_OUTPUT 2

#define MAX_REPORTS     6
#define MAX_REPORT_DATA 20 /* default ATT MTU 23 - 3 */

struct report_chr {
    uint16_t value_handle;
    uint16_t end_handle; /* last handle before the next characteristic */
    uint16_t ccc_handle;
    uint16_t ref_handle;
    uint8_t id;
    uint8_t type;
    struct bt_gatt_subscribe_params sub;
};

struct usb_pkt {
    uint8_t id;
    uint8_t len;
    uint8_t data[MAX_REPORT_DATA];
};

/* ---- state ---- */
static struct bt_conn *peer_conn;
static bool bt_ready;
static bool scanning;
static bool link_ready; /* discovered and subscribed */

static bool cfg_peer_set;
static bt_addr_le_t cfg_peer;

static bool bonded;
static bt_addr_le_t bond_addr;

static struct report_chr reports[MAX_REPORTS];
static int n_reports;
static uint16_t led_handle;

static volatile uint8_t host_leds;

/* ---- work: everything that can block runs here, never in the BT RX thread ---- */
K_THREAD_STACK_DEFINE(bridge_stack, 1536);
static struct k_work_q bridge_q;

K_MSGQ_DEFINE(usb_msgq, sizeof(struct usb_pkt), 16, 4);
static struct k_work usb_work;
static struct k_work led_work;
static struct k_work release_work;
static struct k_work_delayable scan_work;

static void schedule_scan(uint32_t delay_ms) {
    k_work_reschedule_for_queue(&bridge_q, &scan_work, K_MSEC(delay_ms));
}

/* ====================================================================== */
/* USB side                                                               */
/* ====================================================================== */

static void forward_keyboard(const uint8_t *data, uint8_t len) {
    struct zmk_hid_keyboard_report *r = zmk_hid_get_keyboard_report();

    if (len != sizeof(r->body)) {
        LOG_WRN("keyboard report is %d bytes, expected %d: HID options differ between "
                "keyboard and dongle",
                len, (int)sizeof(r->body));
        return;
    }
    memcpy(&r->body, data, len);
    int err = zmk_usb_hid_send_keyboard_report();
    if (err) {
        LOG_DBG("USB keyboard report not sent (%d)", err);
    }
}

static void forward_consumer(const uint8_t *data, uint8_t len) {
    struct zmk_hid_consumer_report *r = zmk_hid_get_consumer_report();

    if (len != sizeof(r->body)) {
        LOG_WRN("consumer report is %d bytes, expected %d: HID options differ between "
                "keyboard and dongle",
                len, (int)sizeof(r->body));
        return;
    }
    memcpy(&r->body, data, len);
    int err = zmk_usb_hid_send_consumer_report();
    if (err) {
        LOG_DBG("USB consumer report not sent (%d)", err);
    }
}

static void usb_work_handler(struct k_work *work) {
    struct usb_pkt pkt;

    while (k_msgq_get(&usb_msgq, &pkt, K_NO_WAIT) == 0) {
        switch (pkt.id) {
        case ZMK_HID_REPORT_ID_KEYBOARD:
            forward_keyboard(pkt.data, pkt.len);
            break;
        case ZMK_HID_REPORT_ID_CONSUMER:
            forward_consumer(pkt.data, pkt.len);
            break;
        default:
            LOG_DBG("ignoring input report id %d", pkt.id);
            break;
        }
    }
}

/* The keyboard went away: release everything so no key stays pressed on the PC. */
static void release_work_handler(struct k_work *work) {
    struct zmk_hid_keyboard_report *kb = zmk_hid_get_keyboard_report();
    struct zmk_hid_consumer_report *cons = zmk_hid_get_consumer_report();

    memset(&kb->body, 0, sizeof(kb->body));
    memset(&cons->body, 0, sizeof(cons->body));
    zmk_usb_hid_send_keyboard_report();
    zmk_usb_hid_send_consumer_report();
}

/* ====================================================================== */
/* LED state, PC -> keyboard                                              */
/* ====================================================================== */

static void led_work_handler(struct k_work *work) {
    if (!link_ready || !peer_conn || !led_handle) {
        return;
    }

    uint8_t leds = host_leds;
    int err = bt_gatt_write_without_response(peer_conn, led_handle, &leds, sizeof(leds), false);
    if (err) {
        LOG_DBG("LED write failed (%d)", err);
    }
}

static int indicators_listener(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);

    if (ev) {
        host_leds = ev->indicators;
        k_work_submit_to_queue(&bridge_q, &led_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(bt_bridge_leds, indicators_listener);
ZMK_SUBSCRIPTION(bt_bridge_leds, zmk_hid_indicators_changed);

/* ====================================================================== */
/* Notifications from the keyboard                                        */
/* ====================================================================== */

static uint8_t notify_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                         const void *data, uint16_t length) {
    if (!data) {
        LOG_DBG("unsubscribed from handle %d", params->value_handle);
        return BT_GATT_ITER_STOP;
    }

    for (int i = 0; i < n_reports; i++) {
        if (reports[i].value_handle != params->value_handle) {
            continue;
        }
        if (length > MAX_REPORT_DATA) {
            LOG_WRN("report of %d bytes does not fit, dropped", length);
            return BT_GATT_ITER_CONTINUE;
        }

        struct usb_pkt pkt = {.id = reports[i].id, .len = (uint8_t)length};
        memcpy(pkt.data, data, length);

        if (k_msgq_put(&usb_msgq, &pkt, K_NO_WAIT)) {
            LOG_WRN("USB queue full, report dropped");
        }
        k_work_submit_to_queue(&bridge_q, &usb_work);
        break;
    }

    return BT_GATT_ITER_CONTINUE;
}

static void subscribed_cb(struct bt_conn *conn, uint8_t err,
                          struct bt_gatt_subscribe_params *params) {
    if (err) {
        LOG_ERR("subscribe to handle %d failed (%d)", params->value_handle, err);
    } else {
        LOG_DBG("subscribed to handle %d", params->value_handle);
    }
}

/* ====================================================================== */
/* GATT discovery of the keyboard's HID service                           */
/*   primary service -> characteristics -> descriptors -> read refs -> go */
/* ====================================================================== */

static struct bt_gatt_discover_params disc;
static struct bt_gatt_read_params read_params;
static uint16_t hids_end;
static int desc_idx;
static int ref_idx;

static void start_discovery(struct bt_conn *conn);
static void discover_next_descriptors(struct bt_conn *conn);
static void read_next_ref(struct bt_conn *conn);

static void fail_link(struct bt_conn *conn, const char *why) {
    LOG_ERR("%s: dropping the link", why);
    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static void finish_discovery(struct bt_conn *conn) {
    int subscribed = 0;

    led_handle = 0;

    for (int i = 0; i < n_reports; i++) {
        struct report_chr *r = &reports[i];

        LOG_DBG("report %d: id %d type %d value %d ccc %d", i, r->id, r->type, r->value_handle,
                r->ccc_handle);

        if (r->type == REPORT_TYPE_INPUT && r->ccc_handle) {
            r->sub.notify = notify_cb;
            r->sub.subscribe = subscribed_cb;
            r->sub.value = BT_GATT_CCC_NOTIFY;
            r->sub.value_handle = r->value_handle;
            r->sub.ccc_handle = r->ccc_handle; /* discovered above, no auto-discovery */
            atomic_set_bit(r->sub.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

            int err = bt_gatt_subscribe(conn, &r->sub);
            if (err && err != -EALREADY) {
                LOG_ERR("subscribe failed (%d)", err);
            } else {
                subscribed++;
            }
        } else if (r->type == REPORT_TYPE_OUTPUT && r->id == ZMK_HID_REPORT_ID_LEDS) {
            led_handle = r->value_handle;
        }
    }

    if (!subscribed) {
        fail_link(conn, "no input report to subscribe to");
        return;
    }

    link_ready = true;
    LOG_INF("bridge ready: %d input report(s) subscribed, LED output %s", subscribed,
            led_handle ? "available" : "not available");

    /* Bring the keyboard's LEDs in line with the PC (caps lock may already be on). */
    k_work_submit_to_queue(&bridge_q, &led_work);
}

static uint8_t ref_read_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *params,
                           const void *data, uint16_t length) {
    struct report_chr *r = &reports[ref_idx];

    if (err || !data || length < 2) {
        LOG_WRN("could not read report reference for handle %d (err %d)", r->value_handle, err);
    } else {
        r->id = ((const uint8_t *)data)[0];
        r->type = ((const uint8_t *)data)[1];
    }

    ref_idx++;
    read_next_ref(conn);
    return BT_GATT_ITER_STOP;
}

static void read_next_ref(struct bt_conn *conn) {
    while (ref_idx < n_reports && !reports[ref_idx].ref_handle) {
        ref_idx++;
    }

    if (ref_idx >= n_reports) {
        finish_discovery(conn);
        return;
    }

    read_params.func = ref_read_cb;
    read_params.handle_count = 1;
    read_params.single.handle = reports[ref_idx].ref_handle;
    read_params.single.offset = 0;

    int err = bt_gatt_read(conn, &read_params);
    if (err) {
        fail_link(conn, "report reference read failed");
    }
}

static uint8_t descriptor_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params) {
    if (!attr) {
        desc_idx++;
        discover_next_descriptors(conn);
        return BT_GATT_ITER_STOP;
    }

    if (!bt_uuid_cmp(attr->uuid, BT_UUID_GATT_CCC)) {
        reports[desc_idx].ccc_handle = attr->handle;
    } else if (!bt_uuid_cmp(attr->uuid, BT_UUID_HIDS_REPORT_REF)) {
        reports[desc_idx].ref_handle = attr->handle;
    }

    return BT_GATT_ITER_CONTINUE;
}

static void discover_next_descriptors(struct bt_conn *conn) {
    /* A report with no room for descriptors has none: skip it (a start past
     * the end would make bt_gatt_discover() fail). */
    while (desc_idx < n_reports &&
           reports[desc_idx].value_handle + 1 > reports[desc_idx].end_handle) {
        desc_idx++;
    }

    if (desc_idx >= n_reports) {
        ref_idx = 0;
        read_next_ref(conn);
        return;
    }

    /* Bounded to this characteristic. Zephyr's descriptor discovery does NOT
     * stop at the next characteristic: it skips the declaration and value and
     * keeps going, so an open-ended search would hand every report the CCC
     * and Report Reference of the last one in the service. */
    disc.uuid = NULL;
    disc.func = descriptor_cb;
    disc.start_handle = reports[desc_idx].value_handle + 1;
    disc.end_handle = reports[desc_idx].end_handle;
    disc.type = BT_GATT_DISCOVER_DESCRIPTOR;

    int err = bt_gatt_discover(conn, &disc);
    if (err) {
        fail_link(conn, "descriptor discovery failed");
    }
}

static uint8_t characteristic_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 struct bt_gatt_discover_params *params) {
    if (!attr) {
        if (!n_reports) {
            fail_link(conn, "keyboard has no HID report characteristic");
            return BT_GATT_ITER_STOP;
        }
        /* The last report's descriptors run to the end of the service. */
        if (!reports[n_reports - 1].end_handle) {
            reports[n_reports - 1].end_handle = hids_end;
        }
        desc_idx = 0;
        discover_next_descriptors(conn);
        return BT_GATT_ITER_STOP;
    }

    const struct bt_gatt_chrc *chrc = attr->user_data;

    /* attr->handle is this characteristic's declaration, so the previous
     * report's descriptors end right before it. */
    if (n_reports && !reports[n_reports - 1].end_handle) {
        reports[n_reports - 1].end_handle = attr->handle - 1;
    }

    if (!bt_uuid_cmp(chrc->uuid, BT_UUID_HIDS_REPORT) && n_reports < MAX_REPORTS) {
        memset(&reports[n_reports], 0, sizeof(reports[n_reports]));
        reports[n_reports].value_handle = chrc->value_handle;
        n_reports++;
    }

    return BT_GATT_ITER_CONTINUE;
}

static uint8_t service_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          struct bt_gatt_discover_params *params) {
    if (!attr) {
        fail_link(conn, "keyboard has no HID service");
        return BT_GATT_ITER_STOP;
    }

    const struct bt_gatt_service_val *svc = attr->user_data;

    hids_end = svc->end_handle;

    disc.uuid = NULL;
    disc.func = characteristic_cb;
    disc.start_handle = attr->handle + 1;
    disc.end_handle = hids_end;
    disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;

    int err = bt_gatt_discover(conn, &disc);
    if (err) {
        fail_link(conn, "characteristic discovery failed");
    }
    return BT_GATT_ITER_STOP;
}

static void start_discovery(struct bt_conn *conn) {
    n_reports = 0;
    desc_idx = 0;
    ref_idx = 0;
    led_handle = 0;
    link_ready = false;

    disc.uuid = BT_UUID_HIDS;
    disc.func = service_cb;
    disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    disc.type = BT_GATT_DISCOVER_PRIMARY;

    int err = bt_gatt_discover(conn, &disc);
    if (err) {
        fail_link(conn, "service discovery failed");
    }
}

/* ====================================================================== */
/* Finding, connecting to and securing the keyboard                       */
/* ====================================================================== */

static void bond_cb(const struct bt_bond_info *info, void *user_data) {
    bt_addr_le_copy(&bond_addr, &info->addr);
    bonded = true;
}

static void refresh_bond(void) {
    bonded = false;
    bt_foreach_bond(BT_ID_DEFAULT, bond_cb, NULL);
}

/* Which device may we talk to?
 *   1. a fixed address from Kconfig, if given
 *   2. the bonded keyboard, once paired
 *   3. otherwise the first device that advertises the HID service with the
 *      wanted name: this is the one-time pairing. */
static bool peer_acceptable(const bt_addr_le_t *addr, const struct bridge_ad_info *info) {
    if (cfg_peer_set) {
        return bt_addr_le_cmp(addr, &cfg_peer) == 0;
    }
    if (bonded) {
        return bt_addr_le_cmp(addr, &bond_addr) == 0;
    }
    return info->hid && info->name_ok;
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                    struct net_buf_simple *ad) {
    if (peer_conn) {
        return;
    }
    if (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
        return;
    }

    struct bridge_ad_info info = bridge_ad_scan(ad->data, ad->len, CONFIG_ZMK_BT_BRIDGE_PEER_NAME);

    if (!peer_acceptable(addr, &info)) {
        return;
    }

    char str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(addr, str, sizeof(str));
    LOG_INF("found the keyboard (%s, rssi %d), connecting", str, rssi);

    if (bt_le_scan_stop()) {
        return;
    }
    scanning = false;

    /* 7.5 ms interval, no latency: lowest input delay. */
    int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM(6, 6, 0, 400),
                                &peer_conn);
    if (err) {
        LOG_ERR("connection attempt failed to start (%d)", err);
        peer_conn = NULL;
        schedule_scan(1000);
    }
}

static void scan_work_handler(struct k_work *work) {
    if (!bt_ready || peer_conn || scanning) {
        return;
    }

    refresh_bond();

    int err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, scan_cb);
    if (err) {
        LOG_ERR("scan failed to start (%d), retrying", err);
        schedule_scan(2000);
        return;
    }

    scanning = true;
    LOG_INF("scanning for the keyboard (%s)",
            cfg_peer_set ? "fixed address" : bonded ? "bonded" : "pairing mode: first match");
}

static void connected_cb(struct bt_conn *conn, uint8_t err) {
    if (conn != peer_conn) {
        return;
    }

    if (err) {
        LOG_WRN("connection failed (0x%02x)", err);
        bt_conn_unref(peer_conn);
        peer_conn = NULL;
        schedule_scan(500);
        return;
    }

    LOG_INF("connected, securing the link");

    /* Encrypts with the stored key, or pairs the first time. */
    int sec = bt_conn_set_security(conn, BT_SECURITY_L2);
    if (sec) {
        fail_link(conn, "could not start security");
    }
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason) {
    if (conn != peer_conn) {
        return;
    }

    LOG_INF("keyboard disconnected (reason 0x%02x)", reason);

    link_ready = false;
    led_handle = 0;
    n_reports = 0;

    bt_conn_unref(peer_conn);
    peer_conn = NULL;

    k_work_submit_to_queue(&bridge_q, &release_work);
    schedule_scan(500);
}

static void security_changed_cb(struct bt_conn *conn, bt_security_t level,
                                enum bt_security_err err) {
    if (conn != peer_conn) {
        return;
    }

    if (err) {
        LOG_ERR("security failed (%d): was the keyboard's Bluetooth profile for the dongle "
                "selected and free?",
                err);
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
        return;
    }

    if (level >= BT_SECURITY_L2) {
        LOG_INF("link secured (level %d), discovering the HID service", level);
        start_discovery(conn);
    }
}

BT_CONN_CB_DEFINE(bridge_conn_cb) = {
    .connected = connected_cb,
    .disconnected = disconnected_cb,
    .security_changed = security_changed_cb,
};

static void pairing_complete_cb(struct bt_conn *conn, bool bondedp) {
    LOG_INF("paired with the keyboard (bonded: %s)", bondedp ? "yes" : "no");
}

static void pairing_failed_cb(struct bt_conn *conn, enum bt_security_err reason) {
    LOG_WRN("pairing failed (%d)", reason);
}

static struct bt_conn_auth_info_cb auth_info_cb = {
    .pairing_complete = pairing_complete_cb,
    .pairing_failed = pairing_failed_cb,
};

/* ====================================================================== */
/* Init                                                                   */
/* ====================================================================== */

/* Settings (and with them the stored bond) are loaded by ZMK's main(), after
 * every SYS_INIT has run. Scanning starts from this commit callback, as ZMK's
 * own BLE does, so the bridge never scans before it knows its bond. */
static int bridge_settings_commit(void) {
#if IS_ENABLED(CONFIG_ZMK_BT_BRIDGE_CLEAR_BONDS_ON_START)
    LOG_WRN("clearing the stored Bluetooth bond");
    bt_unpair(BT_ID_DEFAULT, NULL);
#endif

    bt_ready = true;
    schedule_scan(0);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(bt_bridge, "bt_bridge", NULL, NULL, bridge_settings_commit, NULL);

static int bridge_init(void) {
    k_work_queue_start(&bridge_q, bridge_stack, K_THREAD_STACK_SIZEOF(bridge_stack), 5, NULL);
    k_work_init(&usb_work, usb_work_handler);
    k_work_init(&led_work, led_work_handler);
    k_work_init(&release_work, release_work_handler);
    k_work_init_delayable(&scan_work, scan_work_handler);

    if (sizeof(CONFIG_ZMK_BT_BRIDGE_PEER_ADDR) > 1) {
        if (!bt_addr_le_from_str(CONFIG_ZMK_BT_BRIDGE_PEER_ADDR, "random", &cfg_peer) ||
            !bt_addr_le_from_str(CONFIG_ZMK_BT_BRIDGE_PEER_ADDR, "public", &cfg_peer)) {
            cfg_peer_set = true;
        } else {
            LOG_ERR("ZMK_BT_BRIDGE_PEER_ADDR is not a valid address, ignoring it");
        }
    }

    int err = bt_enable(NULL);
    if (err && err != -EALREADY) {
        LOG_ERR("Bluetooth init failed (%d)", err);
        return err;
    }

    bt_conn_auth_info_cb_register(&auth_info_cb);

    /* Scanning starts in bridge_settings_commit(), once settings are loaded. */
    return 0;
}

SYS_INIT(bridge_init, APPLICATION, CONFIG_ZMK_BT_BRIDGE_INIT_PRIORITY);
