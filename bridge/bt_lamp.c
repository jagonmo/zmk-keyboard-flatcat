/*
 * Windows Dynamic Lighting on the receiver dongle.
 *
 *   Windows --USB HID LampArray--> [dongle] --BLE GATT--> keyboard LEDs
 *
 * The dongle exposes the HID LampArray standard on a second USB HID interface
 * (HID_1, next to the keyboard interface HID_0), exactly like zmk-pro-rgb does
 * on a keyboard plugged in by USB, using the same descriptor and reports
 * (zmk_vfx_pro_rgb/lamp_array_hid.h). The colours Windows sends are forwarded
 * to the keyboard's lamp service (zmk_vfx_pro_rgb/lamp_remote.h), where
 * zmk-pro-rgb shows them (CONFIG_RGB_PRO_LAMP_ARRAY_REMOTE on the keyboard).
 *
 * Lamp count and positions come from the keyboard's own LED map
 * (config/rgb_pro_led_map.h), so both sides describe the same lamps.
 *
 * Only changed parts of a frame are sent, in writes as large as the link
 * allows, and a frame is shown on the keyboard only once it is complete. All
 * writes run on bridge_tx_q so they never delay keystrokes.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/spinlock.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>

#include <zmk_vfx_pro_rgb/lamp_array_hid.h>
#include <zmk_vfx_pro_rgb/lamp_remote.h>

#include <rgb_pro_led_map.h> /* key_to_led, key_col, key_row of the keyboard */

#include "bt_bridge_internal.h"

LOG_MODULE_DECLARE(bt_bridge, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(CONFIG_USB_HID_DEVICE_COUNT >= 2,
             "ZMK_BT_BRIDGE_LAMP_ARRAY needs CONFIG_USB_HID_DEVICE_COUNT=2 (HID_0 is the keyboard)");

#define LAMP_COUNT ARRAY_SIZE(key_to_led)
BUILD_ASSERT(ARRAY_SIZE(key_col) == LAMP_COUNT && ARRAY_SIZE(key_row) == LAMP_COUNT,
             "LED map tables have different sizes");
BUILD_ASSERT(LAMP_COUNT <= RGBP_LR_MAX_LAMPS, "lamp index must fit one byte on the wire");

#define LA_PITCH_UM  CONFIG_ZMK_BT_BRIDGE_LAMP_PITCH_UM
#define LA_DEPTH_UM  10000
#define LA_UPDATE_US CONFIG_ZMK_BT_BRIDGE_LAMP_UPDATE_US

/* Lamp ID == LED index in the chain, as on the keyboard. */
static uint8_t lamp_col[LAMP_COUNT];
static uint8_t lamp_row[LAMP_COUNT];
static uint8_t cols, rows;

/* ---- Host (Windows) state ---- */
static struct k_spinlock la_lock;
static struct la_color staging[LAMP_COUNT];
static struct la_color active[LAMP_COUNT];
static bool autonomous = true;
static uint16_t req_lamp_id;

static const struct device *hid_dev;

/* ---- Link to the keyboard ----
 * lamp_conn is only touched on bridge_tx_q. The Bluetooth stack hands a new
 * link over through pending_conn and start_work. */
static atomic_t link_up;              /* set from BT context, read by the tx work */
static struct bt_conn *lamp_conn;     /* own reference, released on bridge_tx_q */
static struct k_spinlock pending_lock;
static struct bt_conn *pending_conn;  /* new link waiting for start_work */
static uint16_t frame_handle, control_handle;
static atomic_t ready;                /* lamp service found on this link */

static struct rgbp_lr_rgb sent[LAMP_COUNT]; /* what the keyboard already has */
static bool sent_valid;
static int sent_autonomous = -1;      /* -1: keyboard state unknown */

/* ---- Connection parameters ----
 * The keyboard asks (5 s after connecting) for a peripheral latency of 30: it
 * may then sleep through 30 connection events whenever it has nothing to send.
 * Keystrokes do not care, but data the dongle sends only arrives when the
 * keyboard wakes up, every 230-465 ms: 2-4 frames a second. While Windows
 * drives the lamps the dongle asks for latency 0 instead, and gives the
 * keyboard its own wish back when Windows lets go (to save its battery). */
static atomic_t streaming;            /* Windows in host mode (read in BT context) */
static struct k_spinlock pref_lock;
static struct bt_le_conn_param peer_pref = BT_LE_CONN_PARAM_INIT(6, 12, 30, 400);
static int lowlat_applied = -1;       /* bridge_tx_q only; -1: not set on this link */
static struct k_work_delayable param_work;
static void param_handler(struct k_work *work);

static struct k_work_delayable tx_work;
static struct k_work start_work;

static void tx_handler(struct k_work *work);
static void start_handler(struct k_work *work);

static void kick(void) { k_work_reschedule_for_queue(&bridge_tx_q, &tx_work, K_NO_WAIT); }

/* ====================================================================== */
/* USB HID LampArray (same behaviour as zmk-pro-rgb's lamp_array.c)       */
/* ====================================================================== */

static int get_array_attributes(int32_t *len, uint8_t **data) {
    static struct la_array_attributes_report r;

    r = (struct la_array_attributes_report){
        .report_id = LA_ID_ARRAY_ATTRIBUTES,
        .lamp_count = LAMP_COUNT,
        .width_um = cols * LA_PITCH_UM,
        .height_um = rows * LA_PITCH_UM,
        .depth_um = LA_DEPTH_UM,
        .kind = LA_KIND_KEYBOARD,
        .min_update_us = LA_UPDATE_US,
    };
    LOG_DBG("LA get array attributes (%d lamps)", (int)LAMP_COUNT);
    *data = (uint8_t *)&r;
    *len = sizeof(r);
    return 0;
}

static int get_attr_response(int32_t *len, uint8_t **data) {
    static struct la_attr_response_report r;
    uint16_t id = req_lamp_id < LAMP_COUNT ? req_lamp_id : 0;

    r = (struct la_attr_response_report){
        .report_id = LA_ID_ATTR_RESPONSE,
        .lamp_id = id,
        .x_um = lamp_col[id] * LA_PITCH_UM + LA_PITCH_UM / 2,
        .y_um = lamp_row[id] * LA_PITCH_UM + LA_PITCH_UM / 2,
        .z_um = 0,
        .update_latency_us = LA_UPDATE_US,
        .purposes = LA_PURPOSE_CONTROL,
        .red_levels = LA_LEVELS_PER_COLOR,
        .green_levels = LA_LEVELS_PER_COLOR,
        .blue_levels = LA_LEVELS_PER_COLOR,
        .intensity_levels = LA_LEVELS_INTENSITY,
        .is_programmable = LA_IS_PROGRAMMABLE,
        .input_binding = 0,
    };

    /* Auto-advance and wrap, so the host can walk the array. */
    req_lamp_id = (id + 1 < LAMP_COUNT) ? id + 1 : 0;
    LOG_DBG("LA get lamp %d (col %d row %d)", id, lamp_col[id], lamp_row[id]);

    *data = (uint8_t *)&r;
    *len = sizeof(r);
    return 0;
}

static int set_attr_request(const uint8_t *data, int32_t len) {
    if (len < (int32_t)sizeof(struct la_attr_request_report)) {
        LOG_WRN("LA set request too short (%d bytes)", (int)len);
        return -EINVAL;
    }
    const struct la_attr_request_report *r = (const void *)data;

    req_lamp_id = r->lamp_id < LAMP_COUNT ? r->lamp_id : 0;
    LOG_DBG("LA set request lamp %d (len %d)", r->lamp_id, (int)len);
    return 0;
}

static int set_multi_update(const uint8_t *data, int32_t len) {
    if (len < (int32_t)sizeof(struct la_multi_update_report)) {
        return -EINVAL;
    }
    const struct la_multi_update_report *r = (const void *)data;
    uint8_t n = MIN(r->lamp_count, LA_MULTI_MAX);
    bool complete = r->flags & LA_FLAG_UPDATE_COMPLETE;

    k_spinlock_key_t key = k_spin_lock(&la_lock);
    for (uint8_t i = 0; i < n; i++) {
        uint16_t id = r->lamp_ids[i];
        if (id < LAMP_COUNT) {
            staging[id] = r->colors[i];
        }
    }
    if (complete) {
        memcpy(active, staging, sizeof(active));
    }
    k_spin_unlock(&la_lock, key);

    if (complete) {
        kick();
    }
    return 0;
}

static int set_range_update(const uint8_t *data, int32_t len) {
    if (len < (int32_t)sizeof(struct la_range_update_report)) {
        return -EINVAL;
    }
    const struct la_range_update_report *r = (const void *)data;
    uint16_t start = r->lamp_id_start, end = r->lamp_id_end;
    bool complete = r->flags & LA_FLAG_UPDATE_COMPLETE;

    if (start > end || end >= LAMP_COUNT) {
        return 0; /* ignored, per the reference firmware */
    }

    k_spinlock_key_t key = k_spin_lock(&la_lock);
    for (uint16_t id = start; id <= end; id++) {
        staging[id] = r->color;
    }
    if (complete) {
        memcpy(active, staging, sizeof(active));
    }
    k_spin_unlock(&la_lock, key);

    if (complete) {
        kick();
    }
    return 0;
}

static int set_control(const uint8_t *data, int32_t len) {
    if (len < (int32_t)sizeof(struct la_control_report)) {
        return -EINVAL;
    }
    const struct la_control_report *r = (const void *)data;
    bool now_autonomous = r->autonomous & 1;

    k_spinlock_key_t key = k_spin_lock(&la_lock);
    bool changed = (autonomous != now_autonomous);
    autonomous = now_autonomous;
    k_spin_unlock(&la_lock, key);
    atomic_set(&streaming, !now_autonomous);

    if (changed) {
        LOG_INF("LampArray: %s mode", now_autonomous ? "autonomous" : "host");
        kick();
    }
    return 0;
}

#define HID_REPORT_TYPE_FEATURE 0x300
#define HID_TYPE_MASK           0xff00
#define HID_ID_MASK             0x00ff

static int get_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    if ((setup->wValue & HID_TYPE_MASK) != HID_REPORT_TYPE_FEATURE) {
        LOG_DBG("LA get_report wValue 0x%04x not a feature report", setup->wValue);
        return -ENOTSUP;
    }
    switch (setup->wValue & HID_ID_MASK) {
    case LA_ID_ARRAY_ATTRIBUTES:
        return get_array_attributes(len, data);
    case LA_ID_ATTR_RESPONSE:
        return get_attr_response(len, data);
    default:
        LOG_WRN("LA get of unhandled report id %d", setup->wValue & HID_ID_MASK);
        return -ENOTSUP;
    }
}

static int set_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    if ((setup->wValue & HID_TYPE_MASK) != HID_REPORT_TYPE_FEATURE) {
        LOG_DBG("LA set_report wValue 0x%04x not a feature report", setup->wValue);
        return -ENOTSUP;
    }
    switch (setup->wValue & HID_ID_MASK) {
    case LA_ID_ATTR_REQUEST:
        return set_attr_request(*data, *len);
    case LA_ID_MULTI_UPDATE:
        return set_multi_update(*data, *len);
    case LA_ID_RANGE_UPDATE:
        return set_range_update(*data, *len);
    case LA_ID_CONTROL:
        return set_control(*data, *len);
    default:
        LOG_WRN("LA set of unhandled report id %d", setup->wValue & HID_ID_MASK);
        return -ENOTSUP;
    }
}

static const struct hid_ops lamp_ops = {
    .get_report = get_report_cb,
    .set_report = set_report_cb,
};

/* Must run before usb_enable(), like ZMK's own HID init. */
static int lamp_usb_init(void) {
    for (int pos = 0; pos < (int)LAMP_COUNT; pos++) {
        uint8_t led = key_to_led[pos];
        if (led < LAMP_COUNT) {
            lamp_col[led] = key_col[pos];
            lamp_row[led] = key_row[pos];
        }
        cols = MAX(cols, key_col[pos] + 1);
        rows = MAX(rows, key_row[pos] + 1);
    }

    k_work_init_delayable(&tx_work, tx_handler);
    k_work_init_delayable(&param_work, param_handler);
    k_work_init(&start_work, start_handler);

    hid_dev = device_get_binding("HID_1");
    if (!hid_dev) {
        LOG_ERR("LampArray: HID_1 not found (is CONFIG_USB_HID_DEVICE_COUNT=2?)");
        return -EINVAL;
    }
    usb_hid_register_device(hid_dev, lamp_array_report_desc, sizeof(lamp_array_report_desc),
                            &lamp_ops);
    usb_hid_init(hid_dev);
    return 0;
}
SYS_INIT(lamp_usb_init, APPLICATION, CONFIG_ZMK_USB_HID_INIT_PRIORITY);

/* Unplugged from the PC: back to autonomous, as the keyboard does on USB. */
static int lamp_usb_listener(const zmk_event_t *eh) {
    const struct zmk_usb_conn_state_changed *ev = as_zmk_usb_conn_state_changed(eh);
    if (!ev || ev->conn_state != ZMK_USB_CONN_NONE) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    k_spinlock_key_t key = k_spin_lock(&la_lock);
    autonomous = true;
    atomic_set(&streaming, 0);
    req_lamp_id = 0;
    memset(staging, 0, sizeof(staging));
    memset(active, 0, sizeof(active));
    k_spin_unlock(&la_lock, key);
    kick();
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(bt_bridge_lamp_usb, lamp_usb_listener);
ZMK_SUBSCRIPTION(bt_bridge_lamp_usb, zmk_usb_conn_state_changed);

/* ====================================================================== */
/* Sending to the keyboard (bridge_tx_q only)                             */
/* ====================================================================== */

static void tx_handler(struct k_work *work) {
    /* The link went down: let go of our reference and forget the keyboard. */
    if (!atomic_get(&link_up)) {
        if (lamp_conn) {
            bt_conn_unref(lamp_conn);
            lamp_conn = NULL;
        }
        atomic_set(&ready, 0);
        sent_valid = false;
        sent_autonomous = -1;
        lowlat_applied = -1;
        return;
    }
    if (!atomic_get(&ready) || !lamp_conn) {
        return;
    }

    /* Latency: its own work item, so the HCI command it sends does not stack on
     * top of this handler's frame buffers. Same queue, runs right after. */
    if (lowlat_applied != (int)atomic_get(&streaming)) {
        k_work_reschedule_for_queue(&bridge_tx_q, &param_work, K_NO_WAIT);
    }

    struct rgbp_lr_rgb cur[LAMP_COUNT];
    k_spinlock_key_t key = k_spin_lock(&la_lock);
    bool auto_now = autonomous;
    for (int i = 0; i < (int)LAMP_COUNT; i++) {
        cur[i] = (struct rgbp_lr_rgb){active[i].r, active[i].g, active[i].b};
    }
    k_spin_unlock(&la_lock, key);

    /* Colours first, so switching to host mode shows the right frame at once. */
    int per = rgbp_lr_lamps_per_write(bt_gatt_get_mtu(lamp_conn));
    uint8_t buf[RGBP_LR_HDR + 3 * 20];
    per = MIN(per, (int)((sizeof(buf) - RGBP_LR_HDR) / 3));

    if (!sent_valid) {
        /* Unknown keyboard contents: make every chunk dirty once. */
        for (int i = 0; i < (int)LAMP_COUNT; i++) {
            sent[i] = cur[i];
            sent[i].r ^= 0xff;
        }
        sent_valid = true;
    }

    int s = rgbp_lr_next_dirty(cur, sent, LAMP_COUNT, 0, per);
    while (s >= 0) {
        int next = rgbp_lr_next_dirty(cur, sent, LAMP_COUNT, s + per, per);
        size_t len = rgbp_lr_encode(buf, sizeof(buf), cur, LAMP_COUNT, s, per, next < 0);
        int err = len ? bt_gatt_write_without_response(lamp_conn, frame_handle, buf, len, false)
                      : -EINVAL;
        if (err) {
            /* Usually no radio buffer free right now: try the rest shortly. */
            LOG_DBG("lamp frame write failed (%d), retrying", err);
            k_work_reschedule_for_queue(&bridge_tx_q, &tx_work, K_MSEC(10));
            return;
        }
        int count = MIN((int)LAMP_COUNT - s, per);
        memcpy(&sent[s], &cur[s], 3u * count);
        s = next;
    }

    if (sent_autonomous != (int)auto_now) {
        uint8_t v = auto_now ? 1 : 0;
        int err = bt_gatt_write_without_response(lamp_conn, control_handle, &v, 1, false);
        if (err) {
            k_work_reschedule_for_queue(&bridge_tx_q, &tx_work, K_MSEC(10));
            return;
        }
        sent_autonomous = auto_now;
    }
}

/* Low latency while Windows drives the lamps, the keyboard's own otherwise. */
static void param_handler(struct k_work *work) {
    if (!atomic_get(&link_up) || !atomic_get(&ready) || !lamp_conn) {
        return;
    }
    bool want_low = atomic_get(&streaming);
    if (lowlat_applied == (int)want_low) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&pref_lock);
    struct bt_le_conn_param p = peer_pref;
    k_spin_unlock(&pref_lock, key);
    if (want_low) {
        p.latency = 0;
    }

    int err = bt_conn_le_param_update(lamp_conn, &p);
    if (err && err != -EALREADY) {
        LOG_WRN("connection update failed (%d), retrying", err);
        k_work_reschedule_for_queue(&bridge_tx_q, &param_work, K_MSEC(500));
        return;
    }
    lowlat_applied = want_low;
    LOG_INF("Dynamic Lighting %s: peripheral latency %d", want_low ? "active" : "idle", p.latency);
}

/* ====================================================================== */
/* Finding the keyboard's lamp service                                    */
/*   MTU exchange -> primary service -> characteristics -> ready          */
/* ====================================================================== */

static struct bt_uuid_128 uuid_service = BT_UUID_INIT_128(RGBP_LR_UUID_SERVICE);
static struct bt_uuid_128 uuid_frame = BT_UUID_INIT_128(RGBP_LR_UUID_FRAME);
static struct bt_uuid_128 uuid_control = BT_UUID_INIT_128(RGBP_LR_UUID_CONTROL);

static struct bt_gatt_discover_params ldisc;
static struct bt_gatt_exchange_params mtu_params;
static uint16_t svc_end;
static uint16_t found_frame, found_control;

static void lamp_found(struct bt_conn *conn) {
    if (conn != lamp_conn) {
        return; /* answer for a link that is already gone */
    }
    if (!found_frame || !found_control) {
        LOG_WRN("keyboard's lamp service is incomplete; Dynamic Lighting disabled on this link");
        return;
    }
    frame_handle = found_frame;
    control_handle = found_control;
    atomic_set(&ready, 1); /* handles are written before this */
    LOG_INF("lamp service ready (MTU %d, %d lamps per write)", bt_gatt_get_mtu(lamp_conn),
            rgbp_lr_lamps_per_write(bt_gatt_get_mtu(lamp_conn)));
    kick();
}

static uint8_t lamp_chrc_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            struct bt_gatt_discover_params *params) {
    if (!attr) {
        lamp_found(conn);
        return BT_GATT_ITER_STOP;
    }
    const struct bt_gatt_chrc *chrc = attr->user_data;

    if (!bt_uuid_cmp(chrc->uuid, &uuid_frame.uuid)) {
        found_frame = chrc->value_handle;
    } else if (!bt_uuid_cmp(chrc->uuid, &uuid_control.uuid)) {
        found_control = chrc->value_handle;
    }
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t lamp_svc_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params) {
    if (!attr) {
        LOG_INF("keyboard has no lamp service (needs CONFIG_RGB_PRO_LAMP_ARRAY_REMOTE); "
                "Dynamic Lighting disabled, keys unaffected");
        return BT_GATT_ITER_STOP;
    }
    const struct bt_gatt_service_val *svc = attr->user_data;

    svc_end = svc->end_handle;
    ldisc.uuid = NULL;
    ldisc.func = lamp_chrc_cb;
    ldisc.start_handle = attr->handle + 1;
    ldisc.end_handle = svc_end;
    ldisc.type = BT_GATT_DISCOVER_CHARACTERISTIC;

    int err = bt_gatt_discover(conn, &ldisc);
    if (err) {
        LOG_WRN("lamp characteristic discovery failed (%d)", err);
    }
    return BT_GATT_ITER_STOP;
}

static void discover_lamp_service(struct bt_conn *conn) {
    found_frame = 0;
    found_control = 0;

    ldisc.uuid = &uuid_service.uuid;
    ldisc.func = lamp_svc_cb;
    ldisc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    ldisc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    ldisc.type = BT_GATT_DISCOVER_PRIMARY;

    int err = bt_gatt_discover(conn, &ldisc);
    if (err) {
        LOG_WRN("lamp service discovery failed (%d)", err);
    }
}

static void mtu_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params) {
    if (err) {
        LOG_DBG("MTU exchange failed (%d), using the default", err);
    }
    discover_lamp_service(conn);
}

/* On bridge_tx_q: take over the new link from the Bluetooth stack. */
static void start_handler(struct k_work *work) {
    k_spinlock_key_t key = k_spin_lock(&pending_lock);
    struct bt_conn *conn = pending_conn;
    pending_conn = NULL;
    k_spin_unlock(&pending_lock, key);

    if (!conn) {
        return;
    }
    if (lamp_conn) {
        bt_conn_unref(lamp_conn);
    }
    lamp_conn = conn;
    atomic_set(&ready, 0);
    sent_valid = false;
    sent_autonomous = -1;
    lowlat_applied = -1;

    /* Larger writes: up to 19 lamps each instead of 5. */
    mtu_params.func = mtu_cb;
    if (bt_gatt_exchange_mtu(conn, &mtu_params)) {
        discover_lamp_service(conn);
    }
}

void bridge_lamp_start(struct bt_conn *conn) {
    k_spinlock_key_t key = k_spin_lock(&pending_lock);
    if (pending_conn) {
        bt_conn_unref(pending_conn);
    }
    pending_conn = bt_conn_ref(conn);
    k_spin_unlock(&pending_lock, key);

    atomic_set(&link_up, 1);
    k_work_submit_to_queue(&bridge_tx_q, &start_work);
}

bool bridge_lamp_adjust_params(struct bt_le_conn_param *param) {
    k_spinlock_key_t key = k_spin_lock(&pref_lock);
    peer_pref = *param;
    k_spin_unlock(&pref_lock, key);

    if (atomic_get(&streaming)) {
        param->latency = 0;
    }
    return true;
}

void bridge_lamp_stop(void) {
    atomic_set(&link_up, 0);
    k_work_reschedule_for_queue(&bridge_tx_q, &tx_work, K_NO_WAIT);
}
