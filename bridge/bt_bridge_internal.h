/*
 * Shared between the bridge (bt_bridge.c) and its LampArray part (bt_lamp.c).
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
#else
static inline void bridge_lamp_start(struct bt_conn *conn) { ARG_UNUSED(conn); }
static inline void bridge_lamp_stop(void) {}
#endif
