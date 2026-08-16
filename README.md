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
