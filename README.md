# BetterBright

A brightness control plugin for the PSP, for ARK-4 / FasterARK custom firmware.

**Developed by [hobbo91](https://github.com/hobbo91).**

Based on the older *bright3* brightness plugin by hiroi01 (itself a mod
of *bright* by plum) - https://hiroi01.com/?p=prx#bright3 - which is where the
original screen-patching idea came from.

---

## What it does

Press the PSP's **Display (brightness) button** to cycle through the brightness
values you list in `BetterBright.ini`.

It also **remembers** the level you chose and re-applies it after returning to
the XMB, launching a game, or rebooting - instead of snapping back to the
firmware default. (Resuming from sleep is the exception - see Known issues.)

## Config (`BetterBright.ini`)

- One brightness value per line, `0`-`100`.
- Lines starting with `#` are comments.

Two options:

**`combo_mode`** - one optional adjust scheme (the plain Display button always
cycles regardless):

| value | scheme |
|-------|--------|
| `0`   | off (Display button cycling only) |
| `1`   | hold a trigger + tap Display: **R = brighter, L = dimmer** |
| `2`   | hold both triggers + D-pad: **L+R+Up = brighter, L+R+Down = dimmer** |

Both schemes stop at the dimmest/brightest end of your list (no wrap-around).

**`hold_brightness`** - keep the screen at your level on idle. `1` = on
(default), `0` = off.

> The PSP's idle **dim** can't be cancelled by itself - it isn't visible to the
> brightness API, so it can't be detected and undone. The only way to stop it is
> to hold off the display idle timer, and because the dim and the backlight
> **auto-off** are two stages of that *same* timer, stopping the dim also stops
> the off. So `hold_brightness=1` means the screen stays **fully on** (no dim and
> no auto-off); set `0` for normal dimming + auto-off per your Power Save
> settings. **Does not affect auto-sleep**.

## Files

- `BetterBright.prx` - the plugin (build it - see `BUILD.md`).
- `BetterBright.ini` - your settings (keep it next to the .prx).
- `BetterBright.dat` - File the plugin writes to remember your level. 
                       Delete it to reset to "nothing remembered".

## Install (ARK-4 / FasterARK)

Put `BetterBright.prx` and `BetterBright.ini` together in your plugins folder and
enable BetterBright in the ARK Custom Launcher. 

(The included `vsh.txt` / `game.txt` / `pops.txt` in the `legacy` are only for loaders that still use
seplugins-style text files. Do your self a favour, and just use FasterARK)

## Build

See `BUILD.md`. You need the ARK-4 source for one header and one stub library;
everything else is the base PSP SDK.

## Known issues

- **Brightness isn't restored after waking from sleep.** As with the original
  plugin, if something else changes the backlight - most notably the firmware
  setting its own level when you resume from sleep - the plugin can't always see
  it, so it won't put your level back. Just press the Display button (or your
  combo) once to re-apply it.
- This is **only noticeable if you've set a brightness above the firmware
  default** - about **80 on battery, 90 plugged in**. At or below that, the
  firmware's own level looks the same as yours, so you won't see a change.

## Credits

- **hobbo91** - BetterBright.
- **hiroi01** - bright3 (the plugin this is loosely based on).
- **plum** - the original bright.

Use at your own risk.
