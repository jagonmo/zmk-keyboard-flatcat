/*
 * Shared between the bridge (bt_bridge.c) and its optional parts: LampArray
 * (bt_lamp.c) and battery / brightness (bt_status.c).
 */

#pragma once

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>

/* Everything that WRITES to the keyboard over Bluetooth runs on this queue.
 * A write can wait for a free radio buffer, so it must never run on the queue
 * that forwards keystrokes to USB. */
extern struct k_work_q bridge_tx_q;

#if IS_ENABLED(CONFIG_ZMK_BT_BRIDGE_LAMP_ARRAY)
/* The keyboard's HID reports are subscribed: look for its lamp service. Called
 * from the Bluetooth stack's context; only starts asynchronous GATT work. */
void bridge_lamp_start(struct bt_conn *conn);
/* The keyboard disconnected. */
void bridge_lamp_stop(void);
/* The keyboard asks for new connection parameters: remember them, and keep
 * the latency at 0 while Windows drives the lamps. Returns true (accept). */
bool bridge_lamp_adjust_params(struct bt_le_conn_param *param);
#else
static inline void bridge_lamp_start(struct bt_conn *conn) { ARG_UNUSED(conn); }
static inline void bridge_lamp_stop(void) {}
static inline bool bridge_lamp_adjust_params(struct bt_le_conn_param *param) {
    ARG_UNUSED(param);
    return true;
}
#endif

#if IS_ENABLED(CONFIG_ZMK_BT_BRIDGE_STATUS)
/* The keyboard's HID reports are subscribed: look for its battery service and
 * brightness characteristic. Same context and rules as bridge_lamp_start(). */
void bridge_status_start(struct bt_conn *conn);
void bridge_status_stop(void);
#else
static inline void bridge_status_start(struct bt_conn *conn) { ARG_UNUSED(conn); }
static inline void bridge_status_stop(void) {}
#endif
