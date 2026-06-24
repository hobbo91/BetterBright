# BetterBright

Brightness control plugin for the PSP, for ARK-4 / FasterARK custom firmware.

Based on the older *bright3* brightness plugin by hiroi01 (itself a mod
of *bright* by plum) - https://hiroi01.com/?p=prx#bright3 - which is where the
original idea came from. 

---

## What it does

Better control over the PSP display brightness! 

Utilise the full brightness range (0-100), which is otherwise unavailable on a stock PSP. Some models (such as PSP-3000) also allows you to disable the backlight entirely. 

By default the plugin is setup for a PSP-3000 with 8x brightness steps ranging from 0 (off) to 100 (max). 

## How to use

Press the PSP's **Display (brightness) button** to cycle through the brightness
values you list in `BetterBright.ini`. 

(By default) You can also hold hold a trigger + tap Display Button: **R = brighter, L = dimmer** 

It also remembers the level you chose and re-applies it after returning to the XMB, launching a game, 
or rebooting - instead of snapping back to the firmware default. (Resuming from sleep is the exception - see Known issues.)

## Install (ARK-4 / FasterARK)

Put `BetterBright.prx` and `BetterBright.ini` together in your plugins folder and
enable BetterBright in the ARK Custom Launcher. 

https://github.com/PSP-Archive/ARK-4/wiki/Plugins

(The included `vsh.txt` / `game.txt` / `pops.txt` in the `legacy` are only for loaders that still use
seplugins-style text files. Do your self a favour, and just use ARK-4 or FasterARK. 

## Configuration (`BetterBright.ini`)

- One brightness value per line, `0`-`100`. `0=backlight off`, `100=full`, this may vary depending on your PSP model and display.
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

**`hold_brightness`** - keep the screen always on until the console sleeps. `1` = on
, `0` = off (default).

> Intention was to still allow the display to turn off (only prevent dimming)
> The PSP's idle **dim** can't be cancelled by itself - it isn't visible to the
> brightness API, so it can't be detected and undone. The only way to stop it is
> to hold off the display idle timer, and because the dim and the backlight
> **auto-off** are two stages of that *same* timer, stopping the dim also stops
> the off. So `hold_brightness=1` means the screen stays **fully on** (no dim and
> no auto-off); set `0` for normal dimming + auto-off per your Power Save
> settings. **Does not affect auto-sleep**.

## Files

- `BetterBright.prx` - the plugin
- `BetterBright.ini` - your settings (keep it next to the .prx).
- `BetterBright.dat` - File the plugin generates to remember your level. 
                       Delete it to reset to "nothing remembered".

## Build

See `BUILD.md`. You need the ARK-4 source for one header and one stub library;
everything else is the base PSP SDK.

## Use at your own risk!

Whilst the original display is designed to go to its max brightness, the PSP restricts 
this to preserve battery life and "potentially" long-term damage.  

**100% brightness will drain the battery faster!**

In an age of quality 1800/2500mAh batteries, and nobody using UMD any more, battery drain isn't
so much of a concern as was 10+ years ago. 

## Known issues

- **Brightness isn't restored after waking from sleep.** As with the original
  plugin, if something else changes the backlight - most notably the firmware
  setting its own level when you resume from sleep - the plugin can't always see
  it, so it won't put your level back. Just press the Display button (or your
  combo) once to re-apply it.
- This is **only noticeable if you've set a brightness above the firmware
  default** - about **80 on battery, 90 plugged in**. At or below that, the
  firmware's own level looks the same as yours, so you won't see a change.

## Testing

 - All testing has been done using a PSP-3000VB 09G running FasterARK
 - Feedback appreciatd for other models and CFW. 

## Credits

- **hobbo91** - BetterBright.
- **hiroi01** - bright3 (the plugin this is loosely based on).
- **plum** - the original bright.
