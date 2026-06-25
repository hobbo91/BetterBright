# Building BetterBright (v0.4) on macOS

Based on the older *bright3* plugin by hiroi01 (a mod of *bright* by plum); see the README for credits.

This plugin needs **two** things your base PSPDEV install does not ship: an
ARK-4 header (`module2.h`) and the `libpspsystemctrl_kernel.a` stub library.
Both come from the **ARK-4** source tree (the CFW SDK).

You only have to build one tiny stub library once; after that, building the
plugin is a normal `make`.

---

## 0. Prerequisites

You already have PSPDEV (you ran `psp-config --pspdev-path` successfully).
Make sure these are set in the same shell you build from:

https://pspdev.github.io/pspsdk/index.html

```sh
export PSPDEV=/path/to/pspdev     # wherever yours lives
export PATH="$PATH:$PSPDEV/bin"
psp-config --pspsdk-path          # should print a path, no error
```

You also need `git` and `make` (Xcode Command Line Tools provide both).

---

## 1. Get the ARK-4 source and point ARKROOT at it

```sh
git clone https://github.com/PSP-Archive/ARK-4.git ~/ARK-4
export ARKROOT=~/ARK-4
```

---

## 2. Build the stub library

`libpspsystemctrl_kernel.a` is just NID import stubs - it assembles in seconds
and has no heavy dependencies:

```sh
cd "$ARKROOT/libs/SystemCtrlForKernel"
make
ls libpspsystemctrl_kernel.a            # should now exist
```

---

## 3. Build the plugin

```sh
cd /path/to/BetterBright/src
make ARKROOT=~/ARK-4
```

(or just `make` if you `export ARKROOT` first, or edit the `ARKROOT ?=` line at
the top of the Makefile.)

You should get **BetterBright.prx** in that folder.

---

### Note on ARK-4 headers vs the latest pspdev SDK

ARK-4's `systemctrl.h` re-declares a few prototypes it once considered "missing"
from the SDK (e.g. `sceKernelQuerySystemCall`). The current pspdev SDK now also
declares them - with a slightly different return type - so including the whole
`systemctrl.h` fails with a `conflicting types` error. This plugin sidesteps
that entirely: `main.c` pulls in only the self-contained `module2.h` (for
`SceModule2`) and forward-declares the one CFW function it calls. The symbol is
still linked from `libpspsystemctrl_kernel.a`, so no SDK downgrade is needed.

---

## 4. Install on the PSP

Copy to your Memory Stick (or `ef0:` internal storage on a PSP Go):

```
ms0:/seplugins/BetterBright.prx
ms0:/seplugins/BetterBright.ini
```

Enable it for the contexts you want. With ARK-4 the simplest way is the in-XMB
plugin manager (Recovery / VSH menu), or add these lines to the txt files in
`ms0:/seplugins/` (ARK-4 also accepts the classic `path 1` format):

```
ms0:/seplugins/BetterBright.prx 1
```

in `vsh.txt` (XMB), `game.txt` (PSP games), `pops.txt` (PS1 games) as you like.
Reboot for it to take effect.

---
