# FlatCat

ZMK firmware for the **FlatCat**, a 79-key unibody keyboard with per-key RGB
built on an nRF52840 (nice!nano v2 compatible), plus an optional **Bluetooth
receiver dongle** made from a second nice!nano v2.

This repository is both a **ZMK keyboard module** (it defines the `flatcat` and
`flatcatbridge` shields and the receiver code in `bridge/`) and a
**zmk-config** (GitHub Actions builds all the firmware).

## Firmware images

Every push builds these artifacts (see the run's **Artifacts** in Actions):

| Artifact               | Flash it on  | What it is                                               |
|------------------------|--------------|----------------------------------------------------------|
| `flatcat`              | the keyboard | The keyboard firmware. Always needed.                    |
| `flatcat-bridge`       | the dongle   | Bluetooth-to-USB receiver, with Windows Dynamic Lighting. |
| `flatcat-bridge-debug` | the dongle   | Same receiver with a USB serial log, for troubleshooting. |
| `settings-reset`       | either       | Erases stored settings and Bluetooth bonds.              |

To flash: double-tap reset to get the `NICENANO` drive, then copy the `.uf2`.

## Requirements

- **Keyboard:** the FlatCat with its nRF52840 controller.
- **Dongle (optional):** a nice!nano v2 (or another board that builds as
  `nice_nano//zmk`) plugged into the PC.
- **zmk-pro-rgb from its `dongle` branch.** `config/west.yml` already points
  there; that branch has the Bluetooth LampArray receiver the keyboard uses.
- **Windows Dynamic Lighting (optional):** Windows 11 with Dynamic Lighting
  (Settings → Personalization → Dynamic Lighting).
- No local toolchain: everything builds on GitHub Actions.

## Features

### Keyboard

- 79 keys, 6×14 matrix.
- **Per-key RGB:** 36 effects via [zmk-pro-rgb](https://github.com/jagonmo/zmk-pro-rgb).
- Bluetooth with 5 device profiles, plus USB.
- Status indicators drawn over the effects: caps lock, output mode, Bluetooth
  profiles and battery.
- A random effect and colour every time the keyboard wakes from idle.
- Battery reporting to the host.
- Full N-key rollover (NKRO) over USB, Bluetooth and the dongle.

### Receiver dongle

- **Plug-and-play USB keyboard** for PCs without Bluetooth, or wherever
  Bluetooth is unreliable. The keyboard keeps its own firmware; the dongle is
  simply one of its 5 Bluetooth profiles.
- Forwards keystrokes, media keys and the caps lock LED.
- **Bound to one keyboard** by its address after the first pairing. It
  ignores every other device, even another FlatCat.
- **Re-pairs by itself** if the keyboard forgets it (profile cleared or memory
  wiped).
- **Windows Dynamic Lighting:** the dongle shows Windows a LampArray on its USB
  port and streams the colours to the keyboard over Bluetooth.
- **Wakes the PC from sleep** with a key press, like a wired keyboard.

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

## Status indicators

Indicators are drawn on top of whatever is lighting the keys (an effect or
Windows) and always at full brightness, so they stay readable with the
effects dimmed.

### Caps lock: always visible

The **CAPS** key flashes purple twice, then pauses, while caps lock is on.

### Output and Bluetooth: hold **LOWER**

| Key     | Meaning      | Colour                             |
|---------|--------------|------------------------------------|
| `` ` `` | Output mode  | Solid blue = BLE · Solid red = USB |
| `1`–`5` | Profiles 1–5 | See below                          |

The **active** profile shows its live state:

| State                          | Colour         |
|--------------------------------|----------------|
| Connected                      | Solid blue     |
| Paired, currently disconnected | Blinking blue  |
| No device paired               | Blinking green |
| Not the active profile         | Dim white      |

> Profiles other than the active one stay dim: ZMK keeps its profile table
> private to `app/src/ble.c`, so a module can only read the active profile's
> state.

### Battery: hold **LOWER**

**ESC to F9** (10 keys) form a battery gauge. The bar fills in proportion to
the charge; at or below 15 % it is pure red, and above that its colour sweeps
from red to green at 100 %.

## Receiver dongle

### Pairing (once)

1. Flash `flatcat-bridge` on the nice!nano and plug it into the PC.
2. On the keyboard, select a **free** Bluetooth profile (`LOWER` + one of the
   profile keys; clear it with `BT_CLR` first if it was used before).
3. The dongle finds the keyboard by name ("FlatCat"), pairs, and stores the
   keyboard's address. From then on it only talks to that keyboard.

To type through the dongle, select its profile on the keyboard like any other
device. The other four profiles keep working with phones, tablets or other PCs.

### If the keyboard forgets the dongle

Clearing the dongle's profile (`BT_CLR`) or wiping the keyboard's memory is
fine: the dongle sees that the keyboard no longer has the key, drops its own
copy and pairs again with the **same** keyboard. Select a free profile on the
keyboard and wait a few seconds. The keyboard's address is its chip's factory
address, so it does not change when the keyboard's memory is wiped.

### More than one keyboard

Each dongle stays with the keyboard it first paired with. For that first
pairing, only one keyboard named "FlatCat" should be looking for a host.
Alternatively, pin the keyboard's address in `config/flatcatbridge.conf`
(`CONFIG_ZMK_BT_BRIDGE_PEER_ADDR`); `flatcat-bridge-debug` logs it when it
finds the keyboard.

### Pair the dongle with a different keyboard

Flash `settings-reset` on the dongle, then `flatcat-bridge` again: it forgets
the stored keyboard and pairs by name once more.

### What the dongle does not do

- No ZMK Studio and no keymap of its own: all key handling happens on the
  keyboard.
- No lighting of its own: lighting runs on the keyboard; the dongle only
  forwards Windows' colours.

### Waking the PC

A key press wakes a sleeping PC through the dongle. That first key press only
wakes the PC; it is not typed. It needs:

- In Device Manager, the **HID Keyboard Device** under the FlatCat Dongle →
  Properties → Power Management → **Allow this device to wake the computer**.
- USB power kept on during sleep (a BIOS/UEFI setting on some PCs). Works from
  sleep (S3/modern standby); from hibernate or shutdown only if the BIOS keeps
  USB powered and allows USB wake.
- The keyboard still connected to the dongle, i.e. not in its own deep sleep.

### NKRO

Keyboard and dongle both use ZMK's NKRO report, so any number of keys can be
held at once. Limits that come with it:

- NKRO covers the standard key range, up to keypad `=`. `&kp K_SLEEP` (in the
  keymap) is outside it and is no longer sent; F13–F24 and the language
  keys are also out.
- BIOS/UEFI setup screens get a plain 6-key keyboard (USB boot protocol,
  `CONFIG_ZMK_USB_BOOT=y` on keyboard and dongle), so the keyboard works there
  through the dongle. Windows and Linux keep NKRO. A few old BIOSes ignore the
  boot protocol and may still not see it.
- After flashing, hosts that were paired over Bluetooth before may need to be
  removed and paired again, because they cache the old report layout.

## Windows Dynamic Lighting

```
Windows ──USB (HID LampArray)──▶ dongle ──Bluetooth──▶ keyboard LEDs
```

- The dongle reports 79 lamps with each key's position, taken from the same LED
  map the keyboard uses (`config/rgb_pro_led_map.h`).
- Windows' colours show **only while the dongle's profile is the active one**
  on the keyboard. Switching to another profile brings the keyboard's own
  effects back, and switching back shows Windows' colours again.
- Overlays (caps lock, Bluetooth status, battery) and the lighting on/off and
  brightness keys work on top of Windows' colours.
- While Windows controls the lights, the dongle keeps the Bluetooth link at
  its fastest setting (7.5 ms, no peripheral latency). When Windows lets go,
  the keyboard's own power-saving setting comes back. The extra radio use is
  small next to 79 LEDs that are already on.

Enabled by `CONFIG_ZMK_BT_BRIDGE_LAMP_ARRAY=y` in `config/flatcatbridge.conf`
and `CONFIG_RGB_PRO_LAMP_ARRAY_REMOTE=y` in `config/flatcat.conf`.

### Limits

- **About 30 frames per second.** The keyboard redraws its LEDs every 33 ms
  (`CONFIG_RGB_PRO_TICK_MS`), and the dongle reports that interval to
  Windows. The Bluetooth link itself is not the bottleneck.
- **Colour steps at low brightness.** Windows' colours are capped by the
  keyboard's brightness (`CONFIG_RGB_PRO_LAMP_ARRAY_APPLY_BRT=y`). At 20 % each
  channel has only about 52 levels, and the LEDs are 8-bit, so slow fades and
  dark gradients look stepped. Raise the brightness for smoother transitions.
- **USB LampArray on the keyboard itself is off** in this configuration.
  zmk-pro-rgb offers either the USB version or the dongle version, not both.
  To light the keyboard from Windows over its own cable instead, replace
  `CONFIG_RGB_PRO_LAMP_ARRAY_REMOTE=y` with `CONFIG_USB_HID_DEVICE_COUNT=2` and
  `CONFIG_RGB_PRO_LAMP_ARRAY=y` in `config/flatcat.conf`.

### Only one lighting app at a time (OpenRGB)

A LampArray device has a single "next lamp to describe" counter, and the
protocol cannot tell programs apart. If two programs read the lamp list at the
same moment they corrupt each other's view. This is how it shows up:

- **Windows does not list the dongle** in Dynamic Lighting while OpenRGB is
  running, even in the background or system tray. Quit OpenRGB completely,
  unplug the dongle and plug it back in.
- **OpenRGB shows the lamps shifted** (painting ESC lights another key, and
  the offset changes between attempts). Press **Rescan devices** in OpenRGB
  once things are quiet; it reads the positions again.

Two helper scripts for Windows, in `tools/` (run them in Windows PowerShell,
`powershell.exe`):

- `openrgb-rescan.ps1`: asks OpenRGB to rescan devices, like pressing the
  button, after a delay. Needs OpenRGB's SDK server (Settings → General → Start
  Server). The script's header shows how to run it at every logon.
- `lamparray-check.ps1`: read-only report of what Windows sees: the LampArray
  HID collections and the devices Dynamic Lighting accepts, with lamp count,
  size and the first lamp positions.

### Other notes

- Windows caches USB descriptors. After flashing the dongle for the first time
  with Dynamic Lighting, unplug and replug it; if it still does not appear,
  remove it in Device Manager and plug it in again.
- The keyboard gains a small Bluetooth service for the colours. It sits after
  the keyboard (HID) service, so hosts that are already paired keep working;
  if one misbehaves after updating, remove and re-pair it.

## Troubleshooting the dongle

Flash `flatcat-bridge-debug`, open the new USB serial port (any baud rate),
reproduce the problem and keep the output. Useful lines:

| Log line                                      | Meaning                                         |
|-----------------------------------------------|-------------------------------------------------|
| `scanning for ...`                            | Which keyboard it is looking for                |
| `found the keyboard (AA:BB:...)`              | The keyboard's address                          |
| `paired with the keyboard` / `bound to ...`   | First pairing done                              |
| `lost its key: dropping our bond`             | Keyboard forgot the dongle; re-pairing          |
| `bridge ready: N input report(s) subscribed`  | Keys flow from here on                          |
| `lamp service ready (MTU ..., ... per write)` | Dynamic Lighting link is up                     |
| `Dynamic Lighting active: interval 6-6, peripheral latency 0` | Windows took control of the lights |
| `link: interval ... ms, peripheral latency ...`    | Current Bluetooth timing                   |
| `LA get lamp ...` / `LA set request lamp ...` | Each lamp-attribute request from the PC         |

It also prints every thread's stack use every 30 s.

Keyboard and dongle must be built with the same HID options (both use the ZMK
defaults plus `CONFIG_ZMK_HID_INDICATORS=y` and
`CONFIG_ZMK_HID_REPORT_TYPE_NKRO=y`). If they differ, the log says so
and those reports are dropped.

## ZMK Studio

[ZMK Studio](https://zmk.dev/docs/features/studio) lets you rebind keys live
over USB. The build includes Studio's USB transport, but Studio itself is
**off** in this configuration. To turn it on:

1. Uncomment `CONFIG_ZMK_STUDIO=y` in `config/flatcat.conf` and push.
2. Connect the **keyboard** by USB (Studio does not work through the dongle).
3. Open [my.zmk.dev](https://my.zmk.dev) in a Chromium-based browser.
4. Studio starts locked. Press **LOWER + LSHIFT** to unlock.

## Layout

```
ESC  F1  F2  F3  F4  F5  F6  F7  F8  F9  F10 F11 F12 PSCRN
`    1   2   3   4   5   6   7   8   9   0   -   =   DEL
TAB  Q   W   E   R   T   Y   U   I   O   P   [   ]   BKSP
CAPS A   S   D   F   G   H   J   K   L   ;   '   \   ENTER
LSFT #   Z   X   C   V   B   N   M   ,   .   /   UP  RSFT
LCTL LGUI LALT      SPACE          RALT LEFT DOWN RGHT LOWER
```

## Configuration reference

### Keyboard indicators (`config/flatcat.conf`)

Indicators use **key positions** (the order keys appear in the keymap), so you
can move one by changing a number:

```ini
CONFIG_RGB_PRO_CAPS_KEY=42            # CAPS
CONFIG_RGB_PRO_STATUS_OUT_KEY=14      # `
CONFIG_RGB_PRO_STATUS_BT_KEY_BASE=15  # 1 .. 5
CONFIG_RGB_PRO_BATTERY_KEY_START=0    # ESC
CONFIG_RGB_PRO_BATTERY_KEY_COUNT=10   # ESC .. F9
```

Num lock and scroll lock indicators are available but off; uncomment them in
`config/flatcat.conf` and set their key positions to use them.

Set `CONFIG_RGB_PRO_STATUS_OVERLAY=n` or `CONFIG_RGB_PRO_BATTERY_INDICATOR=n`
to turn an indicator off, and `CONFIG_RGB_PRO_RANDOM_ON_WAKE=n` to keep the
same effect after waking. See the
[zmk-pro-rgb docs](https://github.com/jagonmo/zmk-pro-rgb) for the full list.

### Dongle (`config/flatcatbridge.conf`)

| Option                                  | Default     | Meaning                                                    |
|-----------------------------------------|-------------|------------------------------------------------------------|
| `CONFIG_ZMK_BT_BRIDGE`                  | on          | The Bluetooth-to-USB receiver (needs `CONFIG_ZMK_BLE=n`)   |
| `CONFIG_ZMK_BT_BRIDGE_PEER_NAME`        | `"FlatCat"` | Name prefix used for the first pairing only               |
| `CONFIG_ZMK_BT_BRIDGE_PEER_ADDR`        | empty       | Pin one keyboard by address, e.g. `"AA:BB:CC:DD:EE:FF"`    |
| `CONFIG_ZMK_BT_BRIDGE_CLEAR_BONDS_ON_START` | off     | Forget the keyboard at every start (pairing tests only)   |
| `CONFIG_ZMK_BT_BRIDGE_LAMP_ARRAY`       | on          | Windows Dynamic Lighting (needs `CONFIG_USB_HID_DEVICE_COUNT=2`) |
| `CONFIG_ZMK_BT_BRIDGE_LAMP_PITCH_UM`    | 19050       | Key pitch reported to Windows, in micrometres             |
| `CONFIG_ZMK_BT_BRIDGE_LAMP_UPDATE_US`   | 33000       | Shortest update interval reported to Windows              |

The Bluetooth buffer settings in that file (`BT_BUF_ACL_TX_SIZE`,
`BT_BUF_ACL_TX_COUNT`, `BT_BUF_EVT_RX_COUNT`) are what lets the lamp frames go
out at link speed; leave them as they are.

## How the receiver works

The code lives in `bridge/` and builds only into the dongle:

- `bt_bridge.c`: Bluetooth central. Scans, connects and pairs with the
  keyboard (LE Secure Connections), discovers its HID-over-GATT service,
  subscribes to the input reports and re-sends them through ZMK's USB HID. It
  writes the PC's LED state back and handles re-pairing.
- `bt_lamp.c`: the USB HID LampArray interface for Windows, and the sender that
  forwards only the changed lamps to the keyboard's lamp service, in writes as
  large as the link allows.
- `bt_bridge_ad.h`: advertisement parsing and the choice of which keyboard to
  connect to. Kept free of Zephyr headers so it can be unit-tested on a PC.

On the keyboard side, zmk-pro-rgb (`CONFIG_RGB_PRO_LAMP_ARRAY_REMOTE`) adds the
lamp service and shows the colours.

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
