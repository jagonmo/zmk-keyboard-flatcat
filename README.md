# FlatCat Split (test branch)

Two identical FlatCat PCBs used as one 158-key split keyboard.

| | Left | Right |
|---|---|---|
| Role | Central | Peripheral |
| Keys | 79 (positions 0-78) | 79 (positions 0-78, local) |
| Visual columns | 0-13 | 14-27 |
| LEDs | 79 | 79 |
| Host link, Studio, overlays | ✅ | — |

## Files

```
boards/shields/flatcat_split/
├── flatcat_split.dtsi      shared: matrix, LED strip, behaviour, battery
├── flatcat_left.overlay    central: 158-key transform + physical layout
├── flatcat_right.overlay   peripheral: includes the dtsi only
├── flatcat.keymap          158 bindings per layer (shared base name)
├── Kconfig.shield / .defconfig
└── flatcat_split.zmk.yml
config/
├── flatcat.conf            shared by both halves (base name)
├── flatcat_left.conf       central: Studio + overlays
├── flatcat_right.conf      peripheral: nothing extra
├── rgb_pro_led_map.h       per-side LED map
└── west.yml
```

## How the RGB stays in sync

`&rgb_pro` is declared `BEHAVIOR_LOCALITY_GLOBAL`, so ZMK forwards every
command to the central *and* the peripheral. Toggle, effect, hue, brightness
and speed apply to both halves at once.

Both halves share the same PCB, so `key_to_led[]` is identical. Only
`key_col[]` differs: the right half is shifted to columns 14-27 so a
left-to-right sweep runs across the whole keyboard instead of restarting in
the middle.

## File naming

ZMK strips the `_left` / `_right` suffix to locate shared files, so the
common config and keymap must use the **base** name `flatcat`:

| File | Applies to |
|------|-----------|
| `flatcat.conf` | both halves |
| `flatcat.keymap` | both halves (only the central uses it) |
| `flatcat_left.conf` / `flatcat_right.conf` | that half only |
| `flatcat_left.overlay` / `flatcat_right.overlay` | that half only |

> If the unibody `flatcat.conf` from the main branch is left in place, it
> gets merged into the peripheral build too and leaks `CONFIG_ZMK_STUDIO=y`
> onto a half that has no physical layout, which fails the build.

## Pairing

Flash `flatcat-split-left.uf2` to the left half and
`flatcat-split-right.uf2` to the right. If they were previously paired to
anything else, flash `settings_reset` to both first, then power both on
together.

## Known limitations

- **Reactive effects are per-half.** Each side only sees its own key presses,
  so a splash started on the left does not spread to the right.
- **Animations drift.** Each half advances its own frame counter; toggling
  the effect re-aligns them.
- **Random on wake is off.** Halves idle independently and would roll
  different effects, so the option is unavailable on splits.
