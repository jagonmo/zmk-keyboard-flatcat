# FlatCat ZMK Config

ZMK firmware configuration for the FlatCat keyboard — a 79-key unibody
keyboard powered by a nice!nano v2 (nRF52840).

## Features

- **Per-key RGB** with 36 animated effects via the
  [zmk-pro-rgb](https://github.com/jagonmo/zmk-pro-rgb) module
- Bluetooth 5.0 with 5 device profiles
- USB-C wired mode
- Caps-lock LED indicator (green blink on the CAPS key)

## Building

Push to GitHub and the Actions workflow builds the firmware automatically.
Download the `.uf2` from the build artifacts and flash to your nice!nano.

## RGB Controls (Lower layer)

| Key        | Action          |
|------------|-----------------|
| RGBP_TOG   | Toggle on/off   |
| RGBP_EFF   | Next effect     |
| RGBP_EFR   | Previous effect |
| RGBP_HUI   | Hue up          |
| RGBP_HUD   | Hue down        |
| RGBP_BRI   | Brightness up   |
| RGBP_BRD   | Brightness down |
| RGBP_SPI   | Speed up        |
| RGBP_SPD   | Speed down      |

## License

MIT
