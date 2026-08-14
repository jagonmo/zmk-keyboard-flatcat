# FlatCat

ZMK firmware for the **FlatCat** — a 79-key unibody keyboard with per-key RGB,
powered by a nice!nano v2 (nRF52840).

This repository is both a **ZMK keyboard module** (it defines the `flatcat`
shield) and a **zmk-config** (it builds the firmware via GitHub Actions).

## Features

- 79 keys, 6×14 matrix
- **Per-key RGB** — 36 effects via [zmk-pro-rgb](https://github.com/jagonmo/zmk-pro-rgb)
- Bluetooth with 5 device profiles + USB
- Caps-lock indicator (green blink on the CAPS key)
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

## Layout

```
ESC  F1  F2  F3  F4  F5  F6  F7  F8  F9  F10 F11 F12 PSCRN
`    1   2   3   4   5   6   7   8   9   0   -   =   DEL
TAB  Q   W   E   R   T   Y   U   I   O   P   [   ]   BKSP
CAPS A   S   D   F   G   H   J   K   L   ;   '   \   ENTER
LSFT #   Z   X   C   V   B   N   M   ,   .   /   UP  RSFT
LCTL LGUI LALT      SPACE          RALT LEFT DOWN RGHT LOWER
```

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
