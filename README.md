# FlatCat

ZMK firmware for the **FlatCat** — a 79-key unibody keyboard with per-key RGB,
powered by a nice!nano v2 (nRF52840).

This repository is both a **ZMK keyboard module** (it defines the `flatcat`
shield) and a **zmk-config** (it builds the firmware via GitHub Actions).

## Features

- 79 keys, 6×14 matrix
- **Per-key RGB** — 36 effects via [zmk-pro-rgb](https://github.com/jagonmo/zmk-pro-rgb)
- Bluetooth with 5 device profiles + USB
- Status indicators: caps lock, Bluetooth profile, output mode, battery
- Random effect and colour every time the keyboard wakes from idle
- **ZMK Studio** support — edit the keymap live over USB
- Battery reporting

## Building

Push to GitHub — the Actions workflow builds `flatcat.uf2` automatically.
Download it from the run's artifacts and flash it to your nice!nano.

## RGB controls (Lower layer)

| Command    | Action          |
|------------|-----------------|
| `RGBP_TOG` | Toggle on/off   |
| `RGBP_EFF` | Next effect     |
| `RGBP_EFR` | Previous effect |
| `RGBP_HUI` | Hue up          |
| `RGBP_HUD` | Hue down        |
| `RGBP_BRI` | Brightness up   |
| `RGBP_BRD` | Brightness down |
| `RGBP_SPI` | Speed up        |
| `RGBP_SPD` | Speed down      |

## ZMK Studio

The firmware is built with [ZMK Studio](https://zmk.dev/docs/features/studio)
support, so you can rebind keys live over USB without recompiling.

1. Connect the keyboard by USB.
2. Open [my.zmk.dev](https://my.zmk.dev) in a Chromium-based browser.
3. Studio starts locked. Press **LOWER + LSHIFT** to unlock, then edit away.

The unlock key is bound to `&studio_unlock` on the Lower layer. To skip the
lock entirely, set `CONFIG_ZMK_STUDIO_LOCKING=n` in `config/flatcat.conf`.

## Random on wake

Every time the keyboard wakes from idle it jumps to a random effect, so the
lighting is different each time you sit down. Single-colour effects also get
a random hue; rainbow effects keep their own palette.

Turn it off with `CONFIG_RGB_PRO_RANDOM_ON_WAKE=n` in `config/flatcat.conf`.

## Status indicators

Indicators are drawn on top of whatever RGB effect is running and always show
at full brightness, so they stay readable even with the effects dimmed down.

### Caps lock — always visible

The **CAPS** key blinks green once per second while caps lock is active.

### Output and Bluetooth — hold **LOWER**

The second row doubles as a Bluetooth dashboard while the Lower layer is held:

| Key   | Meaning              | Colour                                 |
|-------|----------------------|----------------------------------------|
| `` ` `` | Output mode        | Solid blue = BLE · Solid red = USB     |
| `1`–`5` | Profiles 1–5       | See below                              |

The **active** profile shows its live state:

| State                        | Colour           |
|------------------------------|------------------|
| Connected                    | Solid blue       |
| Paired, currently disconnected | Blinking blue  |
| No device paired             | Blinking green   |
| Not the active profile       | Dim white        |

> Profiles other than the active one stay dim: ZMK keeps its profile table
> private to `app/src/ble.c`, so a module can only read the active profile's
> state.

### Battery — hold **LOWER**

**ESC F1 F2 F3** form a four-segment battery gauge. The bar fills in
proportion to the charge and its colour sweeps from red when empty to green
when full.

## Layout

```
ESC  F1  F2  F3  F4  F5  F6  F7  F8  F9  F10 F11 F12 PSCRN
`    1   2   3   4   5   6   7   8   9   0   -   =   DEL
TAB  Q   W   E   R   T   Y   U   I   O   P   [   ]   BKSP
CAPS A   S   D   F   G   H   J   K   L   ;   '   \   ENTER
LSFT #   Z   X   C   V   B   N   M   ,   .   /   UP  RSFT
LCTL LGUI LALT      SPACE          RALT LEFT DOWN RGHT LOWER
```

## Tweaking the indicators

Everything is set in `config/flatcat.conf` using **key positions** (the order
keys appear in the keymap), so you can move an indicator by changing a number:

```ini
CONFIG_RGB_PRO_CAPS_KEY=42          # CAPS
CONFIG_RGB_PRO_STATUS_OUT_KEY=14    # `
CONFIG_RGB_PRO_STATUS_BT_KEY_BASE=15  # 1 .. 5
CONFIG_RGB_PRO_BATTERY_KEY_START=0    # ESC
CONFIG_RGB_PRO_BATTERY_KEY_COUNT=4    # ESC F1 F2 F3
```

Set `CONFIG_RGB_PRO_STATUS_OVERLAY=n` or `CONFIG_RGB_PRO_BATTERY_INDICATOR=n`
to turn an indicator off. See the
[zmk-pro-rgb docs](https://github.com/jagonmo/zmk-pro-rgb) for the full list.

## Receiver dongle (Bluetooth to USB)

A nice!nano v2 can act as a USB receiver for the keyboard (`flatcat-bridge`).
The keyboard keeps its normal firmware (`flatcat`): the dongle is just one more
entry in its Bluetooth profile list, and the other profiles keep working with
any other device.

The dongle connects only to the keyboard and re-sends its keystrokes, media
keys and caps lock state over USB. Lighting, overlays, layers and battery all
run on the keyboard as usual.

### Pairing (once)

1. Flash `flatcat-bridge` on the nice!nano and plug it into the PC.
2. On the keyboard, select a **free** Bluetooth profile (`LOWER` + one of the
   profile keys; clear it with `BT_CLR` if it was used before).
3. The dongle finds the keyboard by name ("FlatCat"), pairs, and stores that
   keyboard's address. From then on it is bound to that keyboard: it ignores
   every other device, even another FlatCat.

To use the dongle, select its profile on the keyboard like any other device.

### Windows Dynamic Lighting

The dongle shows Windows a LampArray on its USB port (Settings →
Personalization → Dynamic Lighting) and sends the colours to the keyboard over
Bluetooth. Both sides are on by default: `CONFIG_ZMK_BT_BRIDGE_LAMP_ARRAY` in
`config/flatcatbridge.conf` and `CONFIG_RGB_PRO_LAMP_ARRAY_REMOTE` in
`config/flatcat.conf`.

- Windows' colours show only while the dongle's profile is selected on the
  keyboard. Any other profile (another PC, a phone) brings the keyboard's own
  effects back.
- Overlays (caps lock, BT status, battery) and the lighting on/off and
  brightness keys work on top of Windows' colours.
- Only the lamps that changed are sent, and a frame appears only once it is
  complete. Fast animations depend on the Bluetooth link; the debug log of
  `flatcat-bridge-debug` shows how many lamps fit in each write.
- Windows caches USB descriptors: after flashing the dongle for the first
  time with this, unplug and replug it. If it does not appear, remove it in
  Device Manager and plug it in again.
- The keyboard gains a small Bluetooth service for this. It sits after the
  keyboard (HID) service, so hosts that are already paired keep working; if
  one misbehaves, remove and re-pair it.

### If the keyboard forgets the dongle

Clearing the dongle's profile (`BT_CLR`) or wiping the keyboard's memory is fine:
the dongle notices that the keyboard no longer has the key, drops its own copy
and pairs again with the same keyboard. Select a free profile on the keyboard
and wait a few seconds. The keyboard's address is its chip's factory address,
so it does not change when the keyboard's memory is wiped.

### More than one keyboard

Each dongle stays with the keyboard it first paired with. For that first
pairing, only one keyboard named "FlatCat" should be looking for a host, or set
the keyboard's address in `config/flatcatbridge.conf`
(`CONFIG_ZMK_BT_BRIDGE_PEER_ADDR`); `flatcat-bridge-debug` logs it.

### Pair the dongle with a different keyboard

Flash `settings-reset` on the dongle, then `flatcat-bridge` again: it forgets
the stored keyboard and pairs by name once more.

### Problems

`flatcat-bridge-debug` is the same dongle with a USB serial log. Flash it, open
the new serial port, reproduce the problem and keep the output: it shows each
step (found, connected, secured, subscribed) and why one failed.

Keyboard and dongle must be built with the same HID options (both use the
defaults plus `CONFIG_ZMK_HID_INDICATORS=y`). If they differ, the log says so
and those reports are dropped.

## Using the FlatCat shield in your own config

Add this repo as a module in your `config/west.yml`:

```yaml
  projects:
    - name: zmk-keyboard-flatcat
      remote: jagonmo
      revision: main
```

## License

MIT
