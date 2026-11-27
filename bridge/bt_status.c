/*
 * Keyboard status on the receiver dongle: battery level and PC-set brightness.
 *
 *   keyboard Battery Service --BLE--> [dongle] --USB HID--> PC   (battery)
 *   PC --USB HID feature report--> [dongle] --BLE--> keyboard    (brightness)
 *
 * A USB HID interface of its own (HID_2 next to the keyboard and LampArray;
 * HID_1 without LampArray) with two top-level collections:
 *
 *   1. Consumer Control collection with a standard "Battery Strength" input
 *      (Generic Device Controls 0x06:0x20, 0-100 %), report ID 1. Linux turns
 *      this into a normal battery (upower / desktop battery indicator). It is
 *      also readable with GET_REPORT.
 *   2. Vendor collection (usage page 0xFF00) for scripts on the PC:
 *        report ID 2, feature, read: battery level, 255 = unknown / keyboard
 *                                    not connected
 *        report ID 3, feature, read/write: brightness 0-100 %, 255 = never set
 *      Windows opens vendor collections without exclusive access, so
 *      HidD_GetFeature / HidD_SetFeature work from PowerShell (see
 *      tools/flatcat-status.ps1).
 *
 * On the keyboard side the battery comes from ZMK's standard Battery Service
 * (read once, then notifications) and the brightness goes to the BRIGHTNESS
 * characteristic of zmk-pro-rgb's lamp service (lamp_remote.h). The last
 * brightness the PC asked for is sent again on every new link, because the
 * keyboard does not keep its brightness across a restart.
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

#include <zmk_vfx_pro_rgb/lamp_remote.h>

#include "bt_bridge_internal.h"

LOG_MODULE_DECLARE(bt_bridge, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_BT_BRIDGE_LAMP_ARRAY)
#define STATUS_HID_NAME "HID_2"
#define STATUS_HID_MIN  3
#else
#define STATUS_HID_NAME "HID_1"
#define STATUS_HID_MIN  2
#endif
BUILD_ASSERT(CONFIG_USB_HID_DEVICE_COUNT >= STATUS_HID_MIN,
             "ZMK_BT_BRIDGE_STATUS needs one more USB HID interface (CONFIG_USB_HID_DEVICE_COUNT)");

#define ID_BATTERY_INPUT   1
#define ID_BATTERY_FEATURE 2
#define ID_BRIGHTNESS      3

#define LEVEL_UNKNOWN 0xff

static const uint8_t status_report_desc[] = {
    /* 1: standard battery, for operating systems that read it */
    0x05, 0x0C,       /* Usage Page (Consumer)                */
    0x09, 0x01,       /* Usage (Consumer Control)             */
    0xA1, 0x01,       /* Collection (Application)             */
    0x85, ID_BATTERY_INPUT,
    0x05, 0x06,       /*   Usage Page (Generic Device Controls) */
    0x09, 0x20,       /*   Usage (Battery Strength)           */
    0x15, 0x00,       /*   Logical Minimum (0)                */
    0x25, 0x64,       /*   Logical Maximum (100)              */
    0x75, 0x08,       /*   Report Size (8)                    */
    0x95, 0x01,       /*   Report Count (1)                   */
    0x81, 0x02,       /*   Input (Data, Var, Abs)             */
    0xC0,             /* End Collection                       */

    /* 2: vendor collection for PC scripts */
    0x06, 0x00, 0xFF, /* Usage Page (Vendor 0xFF00)           */
    0x09, 0x01,       /* Usage (1)                            */
    0xA1, 0x01,       /* Collection (Application)             */
    0x15, 0x00,       /*   Logical Minimum (0)                */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255)              */
    0x75, 0x08,       /*   Report Size (8)                    */
    0x95, 0x01,       /*   Report Count (1)                   */
    0x85, ID_BATTERY_FEATURE,
    0x09, 0x02,       /*   Usage (2): battery %, 255 unknown  */
    0xB1, 0x02,       /*   Feature (Data, Var, Abs)           */
    0x85, ID_BRIGHTNESS,
    0x09, 0x03,       /*   Usage (3): brightness %            */
    0xB1, 0x02,       /*   Feature (Data, Var, Abs)           */
    0xC0,             /* End Collection                       */
};

static const struct device *hid_dev;

/* ---- State (written from Bluetooth, USB and work-queue contexts) ---- */
static atomic_t level = ATOMIC_INIT(LEVEL_UNKNOWN); /* keyboard battery      */
static atomic_t brightness = ATOMIC_INIT(LEVEL_UNKNOWN); /* last PC request */
static atomic_t brt_dirty;                          /* not yet on keyboard    */
static atomic_t input_dirty;                        /* battery input to send  */

/* ---- Link (owned by bridge_tx_q) ---- */
static atomic_t link_up;
static struct k_spinlock pending_lock;
static struct bt_conn *pending_conn;
static struct bt_conn *st_conn;
static uint16_t brt_handle;

static struct k_work start_work;
static struct k_work_delayable brt_work;
static struct k_work input_work;

/* ====================================================================== */
/* USB side                                                               */
/* ====================================================================== */

/* Battery input report to the PC. Never while suspended: this must not wake
 * the computer (only keystrokes do). Sent later instead. */
static void input_handler(struct k_work *work) {
    if (!atomic_get(&input_dirty)) {
        return;
    }
    uint8_t lv = (uint8_t)atomic_get(&level);
    if (lv == LEVEL_UNKNOWN || !zmk_usb_is_hid_ready() ||
        zmk_usb_get_status() == USB_DC_SUSPEND) {
        return;
    }
    static uint8_t report[2];
    report[0] = ID_BATTERY_INPUT;
    report[1] = lv;
    int err = hid_int_ep_write(hid_dev, report, sizeof(report), NULL);
    if (err) {
        LOG_DBG("battery report not sent (%d), retrying", err);
        k_work_submit_to_queue(&bridge_tx_q, &input_work);
        return;
    }
    atomic_set(&input_dirty, 0);
}

#define HID_REPORT_TYPE_INPUT   0x100
#define HID_REPORT_TYPE_FEATURE 0x300
#define HID_TYPE_MASK           0xff00
#define HID_ID_MASK             0x00ff

static int get_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    static uint8_t r[2];
    uint16_t type = setup->wValue & HID_TYPE_MASK;
    uint8_t id = setup->wValue & HID_ID_MASK;

    r[0] = id;
    if (type == HID_REPORT_TYPE_INPUT && id == ID_BATTERY_INPUT) {
        uint8_t lv = (uint8_t)atomic_get(&level);
        r[1] = lv == LEVEL_UNKNOWN ? 0 : lv; /* this report's range is 0-100 */
    } else if (type == HID_REPORT_TYPE_FEATURE && id == ID_BATTERY_FEATURE) {
        r[1] = (uint8_t)atomic_get(&level);
    } else if (type == HID_REPORT_TYPE_FEATURE && id == ID_BRIGHTNESS) {
        r[1] = (uint8_t)atomic_get(&brightness);
    } else {
        return -ENOTSUP;
    }
    *data = r;
    *len = sizeof(r);
    return 0;
}

static int set_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    if ((setup->wValue & HID_TYPE_MASK) != HID_REPORT_TYPE_FEATURE ||
        (setup->wValue & HID_ID_MASK) != ID_BRIGHTNESS || *len < 2 ||
        (*data)[0] != ID_BRIGHTNESS) {
        return -ENOTSUP;
    }
    uint8_t v = (*data)[1];
    if (v > 100) {
        return -EINVAL;
    }
    atomic_set(&brightness, v);
    atomic_set(&brt_dirty, 1);
    LOG_INF("PC asks for brightness %d %%", v);
    k_work_reschedule_for_queue(&bridge_tx_q, &brt_work, K_NO_WAIT);
    return 0;
}

static const struct hid_ops status_ops = {
    .get_report = get_report_cb,
    .set_report = set_report_cb,
};

/* Must run before usb_enable(), like ZMK's own HID init. */
static int status_usb_init(void) {
    k_work_init(&input_work, input_handler);

    hid_dev = device_get_binding(STATUS_HID_NAME);
    if (!hid_dev) {
        LOG_ERR("status: %s not found (CONFIG_USB_HID_DEVICE_COUNT too small?)",
                STATUS_HID_NAME);
        return -EINVAL;
    }
    usb_hid_register_device(hid_dev, status_report_desc, sizeof(status_report_desc),
                            &status_ops);
    usb_hid_init(hid_dev);
    return 0;
}

/* Plugged in / resumed: the PC may not have the current level yet. */
static int status_usb_listener(const zmk_event_t *eh) {
    const struct zmk_usb_conn_state_changed *ev = as_zmk_usb_conn_state_changed(eh);
    if (ev && ev->conn_state == ZMK_USB_CONN_HID) {
        atomic_set(&input_dirty, 1);
        k_work_submit_to_queue(&bridge_tx_q, &input_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(bt_bridge_status_usb, status_usb_listener);
ZMK_SUBSCRIPTION(bt_bridge_status_usb, zmk_usb_conn_state_changed);

/* ====================================================================== */
/* Keyboard side                                                          */
/*   battery service -> level characteristic -> CCC -> read + subscribe   */
/*   -> BRIGHTNESS characteristic of the lamp service -> ready            */
/* ====================================================================== */

static void set_level(uint8_t lv) {
    if (lv > 100) {
        return;
    }
    if ((uint8_t)atomic_set(&level, lv) != lv) {
        LOG_INF("keyboard battery %d %%", lv);
        atomic_set(&input_dirty, 1);
        k_work_submit_to_queue(&bridge_tx_q, &input_work);
    }
}

static uint8_t bas_notify_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                             const void *data, uint16_t length) {
    if (!data) {
        return BT_GATT_ITER_STOP;
    }
    if (length >= 1) {
        set_level(((const uint8_t *)data)[0]);
    }
    return BT_GATT_ITER_CONTINUE;
}

static struct bt_gatt_discover_params sdisc;
static struct bt_gatt_read_params sread;
static struct bt_gatt_subscribe_params bas_sub;
static uint16_t bas_end, bas_value;
static struct bt_uuid_128 uuid_brightness = BT_UUID_INIT_128(RGBP_LR_UUID_BRIGHTNESS);

static void discover_brightness(struct bt_conn *conn);

static uint8_t bas_read_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *params,
                           const void *data, uint16_t length) {
    if (!err && data && length >= 1) {
        set_level(((const uint8_t *)data)[0]);
    }
    return BT_GATT_ITER_STOP;
}

static uint8_t bas_ccc_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          struct bt_gatt_discover_params *params) {
    if (conn != st_conn) {
        return BT_GATT_ITER_STOP;
    }
    if (attr) {
        bas_sub.notify = bas_notify_cb;
        bas_sub.value = BT_GATT_CCC_NOTIFY;
        bas_sub.value_handle = bas_value;
        bas_sub.ccc_handle = attr->handle;
        atomic_set_bit(bas_sub.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
        int err = bt_gatt_subscribe(conn, &bas_sub);
        if (err && err != -EALREADY) {
            LOG_WRN("battery subscribe failed (%d)", err);
        }
    } else {
        LOG_INF("keyboard battery has no notifications; level read once per link");
    }

    sread.func = bas_read_cb;
    sread.handle_count = 1;
    sread.single.handle = bas_value;
    sread.single.offset = 0;
    if (bt_gatt_read(conn, &sread)) {
        LOG_WRN("battery read failed");
    }

    discover_brightness(conn);
    return BT_GATT_ITER_STOP;
}

static uint8_t bas_chrc_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params) {
    if (conn != st_conn) {
        return BT_GATT_ITER_STOP;
    }
    if (!attr) {
        LOG_INF("keyboard battery service has no level characteristic");
        discover_brightness(conn);
        return BT_GATT_ITER_STOP;
    }
    const struct bt_gatt_chrc *chrc = attr->user_data;
    bas_value = chrc->value_handle;

    sdisc.uuid = BT_UUID_GATT_CCC;
    sdisc.func = bas_ccc_cb;
    sdisc.start_handle = bas_value + 1;
    sdisc.end_handle = bas_end; /* never past the battery service */
    sdisc.type = BT_GATT_DISCOVER_DESCRIPTOR;
    if (bas_value + 1 > bas_end || bt_gatt_discover(conn, &sdisc)) {
        bas_ccc_cb(conn, NULL, &sdisc);
    }
    return BT_GATT_ITER_STOP;
}

static uint8_t bas_svc_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          struct bt_gatt_discover_params *params) {
    if (conn != st_conn) {
        return BT_GATT_ITER_STOP;
    }
    if (!attr) {
        LOG_INF("keyboard has no battery service");
        discover_brightness(conn);
        return BT_GATT_ITER_STOP;
    }
    const struct bt_gatt_service_val *svc = attr->user_data;
    bas_end = svc->end_handle;

    sdisc.uuid = BT_UUID_BAS_BATTERY_LEVEL;
    sdisc.func = bas_chrc_cb;
    sdisc.start_handle = attr->handle + 1;
    sdisc.end_handle = bas_end;
    sdisc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
    if (bt_gatt_discover(conn, &sdisc)) {
        discover_brightness(conn);
    }
    return BT_GATT_ITER_STOP;
}

static uint8_t brt_chrc_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params) {
    if (conn != st_conn) {
        return BT_GATT_ITER_STOP;
    }
    if (!attr) {
        LOG_INF("keyboard cannot take brightness from the PC (zmk-pro-rgb too old, or "
                "RGB_PRO_LAMP_ARRAY_REMOTE off)");
        return BT_GATT_ITER_STOP;
    }
    const struct bt_gatt_chrc *chrc = attr->user_data;
    brt_handle = chrc->value_handle;
    LOG_DBG("brightness characteristic at %d", brt_handle);

    /* New link: the keyboard may have restarted and lost the PC's value. */
    if ((uint8_t)atomic_get(&brightness) != LEVEL_UNKNOWN) {
        atomic_set(&brt_dirty, 1);
    }
    k_work_reschedule_for_queue(&bridge_tx_q, &brt_work, K_NO_WAIT);
    return BT_GATT_ITER_STOP;
}

static void discover_brightness(struct bt_conn *conn) {
    sdisc.uuid = &uuid_brightness.uuid;
    sdisc.func = brt_chrc_cb;
    sdisc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    sdisc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    sdisc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
    if (bt_gatt_discover(conn, &sdisc)) {
        LOG_WRN("brightness discovery failed");
    }
}

/* ---- bridge_tx_q ---- */

static void brt_handler(struct k_work *work) {
    if (!atomic_get(&link_up) || !st_conn || !brt_handle || !atomic_get(&brt_dirty)) {
        return;
    }
    uint8_t v = (uint8_t)atomic_get(&brightness);
    if (v > 100) {
        atomic_set(&brt_dirty, 0);
        return;
    }
    int err = bt_gatt_write_without_response(st_conn, brt_handle, &v, 1, false);
    if (err) {
        k_work_reschedule_for_queue(&bridge_tx_q, &brt_work, K_MSEC(50));
        return;
    }
    atomic_set(&brt_dirty, 0);
    LOG_INF("brightness %d %% sent to the keyboard", v);
}

static void start_handler(struct k_work *work) {
    k_spinlock_key_t key = k_spin_lock(&pending_lock);
    struct bt_conn *conn = pending_conn;
    pending_conn = NULL;
    k_spin_unlock(&pending_lock, key);

    if (!conn) {
        return;
    }
    if (st_conn) {
        bt_conn_unref(st_conn);
    }
    st_conn = conn;
    brt_handle = 0;
    bas_value = 0;

    sdisc.uuid = BT_UUID_BAS;
    sdisc.func = bas_svc_cb;
    sdisc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    sdisc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    sdisc.type = BT_GATT_DISCOVER_PRIMARY;
    if (bt_gatt_discover(conn, &sdisc)) {
        LOG_WRN("battery service discovery failed");
        discover_brightness(conn);
    }
}

static void stop_handler(struct k_work *work) {
    if (atomic_get(&link_up)) {
        return; /* a new link came up meanwhile */
    }
    if (st_conn) {
        bt_conn_unref(st_conn);
        st_conn = NULL;
    }
    brt_handle = 0;
}
static K_WORK_DEFINE(stop_work, stop_handler);

static int status_init(void) {
    k_work_init(&start_work, start_handler);
    k_work_init_delayable(&brt_work, brt_handler);
    return status_usb_init();
}
SYS_INIT(status_init, APPLICATION, CONFIG_ZMK_USB_HID_INIT_PRIORITY);

/* ====================================================================== */
/* Called by the bridge                                                   */
/* ====================================================================== */

void bridge_status_start(struct bt_conn *conn) {
    k_spinlock_key_t key = k_spin_lock(&pending_lock);
    if (pending_conn) {
        bt_conn_unref(pending_conn);
    }
    pending_conn = bt_conn_ref(conn);
    k_spin_unlock(&pending_lock, key);

    atomic_set(&link_up, 1);
    k_work_submit_to_queue(&bridge_tx_q, &start_work);
}

void bridge_status_stop(void) {
    atomic_set(&link_up, 0);
    atomic_set(&level, LEVEL_UNKNOWN); /* the PC script sees "not connected" */
    k_work_submit_to_queue(&bridge_tx_q, &stop_work);
}
