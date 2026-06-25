/*
 * BetterBright  -  brightness control plugin for PSP (CFW: ARK-4 / FasterARK)
 *
 *   v0.9   -   developed by hobbo91  (https://github.com/hobbo91)
 *
 * Based on the older "bright3" brightness plugin by hiroi01 (itself a
 * mod of "bright" by plum): https://hiroi01.com/?p=prx#bright3 - the original
 * screen-patching idea comes from there. BetterBright is otherwise a rework:
 * persistence, sleep/idle handling, two input schemes, and a modern toolchain.
 *
 * Features:
 *   - Press the Display/brightness button to cycle your configured values.
 *   - Remembers your level and re-applies it after XMB return, game launch,
 *     reboot, and resume from sleep.
 *   - Optional idle-dim hold and two optional adjust schemes (combo_mode).
 *   - Optional on-screen "Display Brightness: NN" overlay (osd_enable), shown in
 *     XMB and games via a sceDisplaySetFrameBuf hook (see osd.c).
 */

#include <pspkernel.h>
#include <pspdisplay_kernel.h>   /* sceDisplaySetBrightness / sceDisplayGetBrightness */
#include <pspctrl.h>             /* sceCtrlPeekBufferPositive, PSP_CTRL_* , SceCtrlData */
#include <psppower.h>            /* scePowerTick, PSP_POWER_TICK_DISPLAY               */
#include <pspsdk.h>              /* pspSdkSetK1 - clear syscall guard around the ctrl read */
#include <string.h>

/*
 * We only need SceModule2 and sctrlHENSetStartModuleHandler from the CFW SDK.
 * Including the whole <systemctrl.h> clashes with current pspdev headers: ARK-4
 * once added "missing" prototypes (e.g. sceKernelQuerySystemCall) that upstream
 * pspsdk has since added too, with a different return type. So we pull in just
 * the self-contained struct header and forward-declare the single function we
 * call - the symbol is still resolved at link time from -lpspsystemctrl_kernel.
 */
#include <module2.h>             /* SceModule2 (self-contained, no clashes) */
typedef int (*STMOD_HANDLER)(SceModule2 *);
STMOD_HANDLER sctrlHENSetStartModuleHandler(STMOD_HANDLER new_handler);

#include "conf.h"
#include "memory.h"
#include "osd.h"
#include "log.h"

PSP_MODULE_INFO("BetterBright", 0x1000, 0, 1);

#define MAKE_CALL(a, f) _sw(0x0C000000 | (((u32)(f) >> 2) & 0x03FFFFFF), a);

/* Button combos (held-together masks). */
#define COMBO_UP   (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_UP)
#define COMBO_DOWN (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_DOWN)

/* ---- globals ---- */
Bright *bright_buf = NULL;
int bright_count   = 0;
int cur            = -1;     /* index currently shown; -1 = nothing applied yet */
STMOD_HANDLER previous = NULL;

char data_path[256];         /* full path to BetterBright.dat (next to the plugin)   */
int  saved_level    = -1;    /* last applied brightness 0-100; -1 = none saved  */
int  g_combo_mode    = 0;    /* from ini: 0=off, 1=L/R+Display, 2=L+R+Up/Down   */
int  g_dim_level     = -1;   /* from ini: -1=AUTO (2nd-lowest), else 0-100      */
int  g_keep_display_on = 0;  /* from ini: 1 = never dim AND never auto-off      */
int  g_disable_sleep = 0;    /* from ini: 1 = prevent the auto-sleep timer      */
int  g_osd_enable    = 1;    /* from ini: 1 = show "Display Brightness: NN"     */
int  g_debug_enable  = 0;    /* from ini: 1 = verbose log + DEBUG OSD line      */
volatile int g_running = 1;  /* worker thread run flag                          */
volatile int g_dirty   = 0;  /* "brightness changed, please persist" flag       */

#define BB_VERSION "0.9"
int  g_first_run = 0;        /* 1 = BetterBright.dat was absent at boot          */

/* Write-coalescing: ApplyIndex sets g_dirty and restarts g_save_settle on every
 * change; the worker only writes once it has settled (no change for SAVE_SETTLE
 * loops), so a flurry of presses becomes ONE write. g_disk_* track what's
 * currently on the Memory Stick so we never write a value that's already there. */
#define SAVE_SETTLE 12              /* 12 * 80ms ~= 1s of no change before writing */
volatile int g_save_settle = 0;
int g_disk_level = -2;              /* -2 = unknown (no .dat read yet)             */
int g_disk_index = -2;

/* ----------------------------------------------------------------------------
 * Patch helpers  (identical to the original 0.03)
 * ------------------------------------------------------------------------- */
void ClearCaches(void)
{
	sceKernelDcacheWritebackAll();
	sceKernelIcacheClearAll();
}

int GetPatchAddr(u32 text_addr, u32 text_end, u32 *addr)
{
	for(; text_addr < text_end; text_addr += 4)
	{
		if(
			_lw(text_addr     ) == 0x8FBF0000 &&
			_lw(text_addr +  4) == 0x00801021 &&
			_lw(text_addr +  8) == 0x03E00008 &&
			_lw(text_addr + 12) == 0x27BD0010
		)
		{
			*addr = (text_addr - 12);
			return 0;
		}
	}
	return -1; /* not found */
}

/* ----------------------------------------------------------------------------
 * Brightness application + persistence
 * ------------------------------------------------------------------------- */

/* Apply bright_buf[i] and remember it in RAM. The actual write to BetterBright.dat
 * is deferred to WorkerThread: this function can run inside the display-service
 * hook, where doing file I/O directly is unsafe (privilege/k1 context), which is
 * why the .dat was never being written before. */
static void ApplyIndex(int i)
{
	if(i < 0 || i >= bright_count) return;
	cur         = i;
	saved_level = bright_buf[i].level;
	sceDisplaySetBrightness(saved_level, 0);
	if(g_osd_enable) osd_notify(saved_level);   /* show "Display Brightness: NN" */
	g_dirty       = 1;                 /* ask WorkerThread to persist        */
	g_save_settle = SAVE_SETTLE;       /* ...but not until it stops changing */
}

/* Move one step forward through the configured list (with wrap).
 * Used by the Display button, which is one-directional and so must wrap. */
static void StepForward(void)
{
	int n = (cur < 0) ? 0 : (cur + 1) % bright_count;
	ApplyIndex(n);
}

/* Clamping versions for the L+R combo: step toward an end and STOP there
 * (no wrap-around back to the other end). */
static void StepForwardClamp(void)
{
	int n = (cur < 0) ? 0 : (cur + 1);
	if(n > bright_count - 1) n = bright_count - 1;
	if(n != cur) ApplyIndex(n);                       /* moved -> apply (+OSD) */
	else if(g_osd_enable) osd_notify(saved_level);      /* at max -> still show OSD */
}
static void StepBackwardClamp(void)
{
	int n = (cur < 0) ? 0 : (cur - 1);
	if(n < 0) n = 0;
	if(n != cur) ApplyIndex(n);
	else if(g_osd_enable) osd_notify(saved_level);      /* at min -> still show OSD */
}

/* Re-apply the remembered brightness WITHOUT advancing or re-saving. */
static void ApplySaved(void)
{
	if(saved_level >= 0) sceDisplaySetBrightness(saved_level, 0);
}

/* Our own dimming: drop to the configured dim level WITHOUT touching saved_level,
 * so a wake can restore the real one. dim_level=AUTO (-1) uses the 2nd-lowest
 * configured value; otherwise the explicit 0-100 value is used. We never brighten
 * when "dimming" - if the target isn't below the current level, leave it. */
static void ApplyDim(void)
{
	int dim;
	if(saved_level < 0) return;
	if(g_dim_level >= 0)
		dim = g_dim_level;                                  /* explicit 0-100 */
	else if(bright_count > 0)
		dim = bright_buf[(bright_count > 1) ? 1 : 0].level; /* AUTO: 2nd-lowest */
	else
		return;
	if(dim < saved_level) sceDisplaySetBrightness(dim, 0);
}

/* ----------------------------------------------------------------------------
 * The Display-button hook. Plain press cycles (and wraps) like the original.
 * In combo_mode 1, holding a trigger while pressing it adjusts instead:
 *   L + Display = dimmer (down, clamped),  R + Display = brighter (up, clamped).
 * We read L/R live here, at the moment of the press. The press itself is fresh
 * controller input, so this is reliable even right after a resume from sleep -
 * unlike a value sampled in the background thread, which the firmware leaves
 * stale until the first input after wake. k1 is cleared around the read because
 * this runs in the sceDisplay_Service context (dirty k1).
 * ------------------------------------------------------------------------- */
/* The firmware steps its native backlight level (44/60/72/84) on every real
 * Display press and passes it here.
 *
 * Tracks the firmware's last native level and when the patch last fired, so the
 * one ambiguous transition (84->44) can be split by timing. */
static volatile int g_last_native = -1;
static volatile unsigned int g_last_patch_us = 0;
static volatile int g_dimmed = 0;                 /* 1 = we've applied our own dim   */
static volatile unsigned int g_last_wake_us = 0;  /* guards against re-dim on wake   */

#define WRAP_RECENT_US 5000000u   /* 5s: a real wrap follows a press within this;
                                     a firmware idle-reset comes minutes later */
#define REDIM_GUARD_US 8000000u   /* don't re-dim within 8s of a wake (the wake's own
                                     firmware re-assert must not look like new idle) */
#define ANY_BTN (PSP_CTRL_SELECT|PSP_CTRL_START|PSP_CTRL_UP|PSP_CTRL_RIGHT|PSP_CTRL_DOWN| \
                 PSP_CTRL_LEFT|PSP_CTRL_LTRIGGER|PSP_CTRL_RTRIGGER|PSP_CTRL_TRIANGLE| \
                 PSP_CTRL_CIRCLE|PSP_CTRL_CROSS|PSP_CTRL_SQUARE)

void sceDisplaySetBrightnessPatched(int level, int unk1)
{
	unsigned int now = sceKernelGetSystemTimeLow();
	unsigned int gap = now - g_last_patch_us;
	int prev = g_last_native;
	int press, did_combo = 0;
	(void)unk1;
	g_last_patch_us = now;
	g_last_native   = level;

	/* A real Display press steps the native level UP (44->60->72->84) or wraps
	 * 84->44. The firmware off/idle/wake instead resets DOWN to 44 or repeats the
	 * level. The only collision is 84->44 - a genuine wrap arrives moments after
	 * the previous press, a firmware reset arrives after a long idle. */
	if(prev < 0)                       press = 1;                      /* boot -> press */
	else if(level == prev)             press = 0;                      /* repeat -> firmware */
	else if(prev == 84 && level == 44) press = (gap < WRAP_RECENT_US); /* wrap iff recent */
	else                               press = (level > prev);         /* up=press, down=firmware */

	if(!press)
	{
		/* Firmware idle event. Apply OUR dim once - unless the display is held on,
		 * or we're within a few seconds of a wake (which re-asserts through here). */
		if(!g_keep_display_on && !g_dimmed && (now - g_last_wake_us) > REDIM_GUARD_US)
		{
			ApplyDim();
			g_dimmed = 1;
			if(g_debug_enable && g_osd_enable) osd_debug("dim", level, (unsigned int)saved_level);
		}
		else if(g_debug_enable && g_osd_enable) osd_debug("idle", level, (unsigned int)saved_level);
		return;
	}

	if(g_dimmed)
	{
		/* A press while dimmed = waking via the Display button -> restore, no cycle. */
		g_dimmed = 0;
		g_last_wake_us = now;
		ApplySaved();
		if(g_debug_enable && g_osd_enable) osd_debug("wake", level, (unsigned int)saved_level);
		return;
	}

	if(g_combo_mode == 1)
	{
		SceCtrlData pad;
		u32 k1 = pspSdkSetK1(0);
		int got = sceCtrlPeekBufferPositive(&pad, 1);
		pspSdkSetK1(k1);
		if(got > 0)
		{
			int l = (pad.Buttons & PSP_CTRL_LTRIGGER);
			int r = (pad.Buttons & PSP_CTRL_RTRIGGER);
			if(l && !r)      { StepBackwardClamp(); did_combo = 1; }   /* L+Display = dimmer  */
			else if(r && !l) { StepForwardClamp();  did_combo = 1; }   /* R+Display = brighter */
		}
	}
	if(!did_combo) StepForward();

	if(g_debug_enable && g_osd_enable) osd_debug("press", level, (unsigned int)saved_level);
}

/* Re-assert our saved level for a short window right after the plugin loads, so a
 * fresh boot / game launch keeps the brightness you last set (the firmware sets
 * its own level during init; this puts yours back). */
#define LOAD_TICKS 125            /* 125 * 80ms ~= 10s */
static volatile int g_load_ticks = 0;

/* ----------------------------------------------------------------------------
 * Worker thread (always runs while the plugin is loaded).
 *
 * Load-window persistence: for ~10s after the plugin loads, if the brightness is
 * reported BELOW our saved level (the firmware setting its own level during init)
 * we put ours back. The firmware's init level IS visible to GetBrightness, so
 * this works - it's what makes a fresh boot keep your brightness.
 *
 * keep_display_on: the firmware's idle dim is NOT visible to GetBrightness
 * (confirmed on hardware), so it cannot be caught and undone. The only way to
 * stop it is to reset the display idle timer with scePowerTick. The dim and the
 * backlight auto-off are two stages of that SAME timer, so ticking every loop
 * keeps the screen fully on - no dim, no off. (Splitting them isn't possible:
 * one timer, two stages.)
 *
 * KNOWN LIMIT: the firmware's own level on wake from sleep is likewise invisible
 * to GetBrightness, so it is not restored. See the README known issue.
 *
 * Also persists pending changes and polls combo_mode 2 (combo_mode 1 is in the
 * hook).
 * ------------------------------------------------------------------------- */
int WorkerThread(SceSize args, void *argp)
{
	int up_prev = 0, down_prev = 0;
	unsigned int last_loop_us = 0;
	SceCtrlData pad;
	int have_pad;

	(void)args; (void)argp;

	g_load_ticks = LOAD_TICKS;
	log_msg("worker: started");

	while(g_running)
	{
		unsigned int now_us = sceKernelGetSystemTimeLow();

		/* sample the pad once per loop (k1 cleared: this thread may run dirty). */
		{
			u32 k1 = pspSdkSetK1(0);
			have_pad = (sceCtrlPeekBufferPositive(&pad, 1) > 0);
			pspSdkSetK1(k1);
		}

		/* (0) resume-from-sleep: the loop runs every ~80ms, so a multi-second gap
		 * means the system was suspended (manual sleep). On resume the firmware
		 * leaves brightness at its own level, so re-assert ours. */
		{
			if(last_loop_us != 0 && (now_us - last_loop_us) > 2000000u)
			{
				ApplySaved();
				g_dimmed = 0;
				g_last_wake_us = now_us;
			}
			last_loop_us = now_us;
		}

		/* (0b) wake from our own dim via ANY button (the Display button wakes
		 * through the patch; everything else we catch here, since the pad is
		 * sampled continuously and reliably). */
		if(g_dimmed && have_pad && (pad.Buttons & ANY_BTN))
		{
			ApplySaved();
			g_dimmed = 0;
			g_last_wake_us = sceKernelGetSystemTimeLow();
		}

		/* (a) load-window persistence: re-apply our level for ~10s after load if
		 * the firmware's init lowered it. (now > 0 just skips a transient 0.) */
		if(g_load_ticks > 0)
		{
			if(saved_level >= 0)
			{
				int now = -1, unk = 0;
				sceDisplayGetBrightness(&now, &unk);
				if(now > 0 && now < saved_level)
					sceDisplaySetBrightness(saved_level, 0);
			}
			g_load_ticks--;
		}

		/* (b) keep_display_on: reset the display idle timer every loop so the screen
		 * never dims OR powers off (one timer drives both stages, so this holds the
		 * screen fully on). Off by default - leave it for normal dim + auto-off. */
		if(g_keep_display_on)
			scePowerTick(PSP_POWER_TICK_DISPLAY);

		/* disable_sleep: reset only the suspend (auto-sleep) idle timer, so the PSP
		 * never sleeps on its own. Manual sleep (the power switch) still works. */
		if(g_disable_sleep)
			scePowerTick(PSP_POWER_TICK_SUSPEND);

		/* (c) persist a pending change from a safe context - debounced, and only
		 * if the value differs from what's already on disk. */
		if(g_dirty)
		{
			if(g_save_settle > 0)
				g_save_settle--;
			else
			{
				g_dirty = 0;
				if(saved_level != g_disk_level || cur != g_disk_index)
				{
					SaveBrightness(data_path, saved_level, cur);
					g_disk_level = saved_level;
					g_disk_index = cur;
					if(g_debug_enable) { log_kv("save.level", saved_level); log_kv("save.index", cur); }
				}
			}
		}

		/* (d) tick down the on-screen-display visibility timer (only relevant
		 * when the OSD is enabled - when it's off, leave the subsystem alone) */
		if(g_osd_enable)
			osd_tick();

		/* (e) combo_mode 2 polling (L+R+Up/Down). combo_mode 1 is read in the hook. */
		if(g_combo_mode == 2 && have_pad)
		{
			int up_now   = ((pad.Buttons & COMBO_UP)   == COMBO_UP);
			int down_now = ((pad.Buttons & COMBO_DOWN) == COMBO_DOWN);

			/* rising edge only -> one step per press, clamped at the ends */
			if(up_now   && !up_prev)   StepForwardClamp();  /* L+R+Up   = brighter */
			if(down_now && !down_prev) StepBackwardClamp(); /* L+R+Down = dimmer  */

			up_prev   = up_now;
			down_prev = down_now;
		}

		/* Normally idle ~80ms between checks. During the load window only, poll
		 * brightness once more halfway through that wait, so the firmware's init
		 * dim is undone in ~40ms instead of ~80ms (shorter visible dip). The loop
		 * itself still completes once per ~80ms, so OSD/save/combo timing and the
		 * ~10s window length (g_load_ticks decrements once per loop) are unchanged. */
		if(g_load_ticks > 0)
		{
			sceKernelDelayThread(40000);
			if(saved_level >= 0)
			{
				int b = -1, u = 0;
				sceDisplayGetBrightness(&b, &u);
				if(b > 0 && b < saved_level)
					sceDisplaySetBrightness(saved_level, 0);
			}
			sceKernelDelayThread(40000);
		}
		else
			sceKernelDelayThread(80000);   /* ~80ms loop */
	}

	return sceKernelExitDeleteThread(0);
}

static void StartWorker(void)
{
	SceUID th = sceKernelCreateThread("BetterBright_worker", WorkerThread,
	                                  0x20, 0x2000, 0, NULL);
	if(th >= 0) sceKernelStartThread(th, 0, NULL);
}

/* ----------------------------------------------------------------------------
 * Module hooks
 * ------------------------------------------------------------------------- */
int OnModuleStart(SceModule2 *mod)
{
	if(strcmp(mod->modname, "sceDisplay_Service") == 0)
	{
		u32 patch_addr;
		if(GetPatchAddr(mod->text_addr, mod->text_addr + mod->text_size, &patch_addr) == 0)
		{
			MAKE_CALL(patch_addr, sceDisplaySetBrightnessPatched);
			ClearCaches();
		}
		ApplySaved();
	}
	return previous ? previous(mod) : 0;
}

int module_start(SceSize args, void *argp)
{
	u32 patch_addr;
	char path[256];
	int saved_index = 0;
	SceModule2 *mod;

	/* Derive the BetterBright.dat path from the plugin's own path BEFORE we reuse
	 * path[] for the .ini (GetConfigPath/CountItem mutate it). */
	strcpy(data_path, (char *)argp);
	GetDataPath(data_path);

	/* First run = the .dat doesn't exist yet. We show a one-off credit when it is
	 * actually created (the first brightness change), then never again. */
	{
		u32 k1 = pspSdkSetK1(0);
		SceUID fd = sceIoOpen(data_path, PSP_O_RDONLY, 0777);
		if(fd >= 0) { sceIoClose(fd); g_first_run = 0; }
		else          g_first_run = 1;
		pspSdkSetK1(k1);
	}

	/* Build the BetterBright.ini path and read the configured values + settings. */
	strcpy(path, (char *)argp);
	GetConfigPath(path);

	bright_count = CountItem(path);
	if(bright_count <= 0) return -1;

	bright_buf = (Bright *)memoryAllocEx("ms_malloc", MEMORY_KERN_HI, 0,
	                                     sizeof(Bright) * bright_count, PSP_SMEM_Low, NULL);
	if(!bright_buf) return -1;

	/* ReadItem fills bright_buf and sets every settings field (defaults first). */
	{
		BrightSettings settings;
		ReadItem(path, bright_buf, &settings);
		g_combo_mode       = settings.combo_mode;
		g_dim_level        = settings.dim_level;
		g_keep_display_on  = settings.keep_display_on;
		g_disable_sleep    = settings.disable_sleep;
		g_osd_enable       = settings.osd_enable;
		g_debug_enable     = settings.debug_enable;
		osd_set_style(settings.osd_text_colour, settings.osd_bg_colour,
		              settings.osd_size, settings.osd_position);
	}

	/* Bring up logging as early as we can (the ini is the first thing we know).
	 * All log writes happen here in module_start or in the worker - never in the
	 * display/brightness hooks. */
	log_set_path((char *)argp);
	log_enable(g_debug_enable);
	if(g_debug_enable)
	{
		log_reset();
		log_msg("=== BetterBright v" BB_VERSION " module_start ===");
		log_kv("settings.combo_mode",          g_combo_mode);
		log_kv("settings.dim_level",            g_dim_level);
		log_kv("settings.keep_display_on",      g_keep_display_on);
		log_kv("settings.disable_sleep",        g_disable_sleep);
		log_kv("settings.osd_enable",           g_osd_enable);
		log_kv("settings.first_run",            g_first_run);
		log_kv("ini.bright_count",              bright_count);
	}

	/* Restore remembered brightness (if any) and resume the cycle just after it. */
	if(LoadBrightness(data_path, &saved_level, &saved_index) == 0)
	{
		if(saved_index < 0 || saved_index >= bright_count) saved_index = 0;
		cur = saved_index;
		g_disk_level = saved_level;   /* what we just read is what's on disk */
		g_disk_index = cur;
	}
	log_kv("load.saved_level", saved_level);
	log_kv("load.saved_index", saved_index);

	/* First run (no .dat existed): create it NOW so the credit shows exactly once
	 * and never again, and so we don't depend on the deferred save. We capture the
	 * brightness the screen is already at (mapped to the nearest configured step)
	 * so nothing visibly changes - we're just taking ownership of the value. */
	if(g_first_run)
	{
		int lvl = -1, unk = 0, i, best = 0, bestd = 1000;
		u32 k1 = pspSdkSetK1(0);
		sceDisplayGetBrightness(&lvl, &unk);
		pspSdkSetK1(k1);
		if(lvl < 1 || lvl > 100) lvl = bright_buf[bright_count - 1].level; /* fallback: brightest */

		for(i = 0; i < bright_count; i++)             /* nearest step for cycling */
		{
			int d = bright_buf[i].level - lvl; if(d < 0) d = -d;
			if(d < bestd) { bestd = d; best = i; }
		}

		saved_level  = lvl;
		saved_index  = best;
		cur          = best;
		SaveBrightness(data_path, saved_level, cur);  /* the .dat now exists */
		g_disk_level = saved_level;
		g_disk_index = cur;
		log_kv("firstrun.saved_level", saved_level);
		log_kv("firstrun.saved_index", saved_index);
	}

	mod = (SceModule2 *)sceKernelFindModuleByName("sceDisplay_Service");
	if(mod == NULL) { log_msg("FATAL: sceDisplay_Service not found"); return -1; }
	log_msg("sceDisplay_Service: found");

	if(GetPatchAddr(mod->text_addr, mod->text_addr + mod->text_size, &patch_addr) == 0)
	{
		MAKE_CALL(patch_addr, sceDisplaySetBrightnessPatched);
		ClearCaches();
		log_kx("brightness_patch.addr", patch_addr);
	}
	else
	{
		previous = sctrlHENSetStartModuleHandler(OnModuleStart);
		log_msg("brightness_patch: pattern not found, deferred via module handler");
	}

	/* Only install the framebuffer hook when the OSD is enabled - when it's off
	 * the plugin runs exactly as if this feature didn't exist. sceDisplay_Service
	 * is confirmed present above, so the export lookup succeeds here. The OSD now
	 * appears only when you actually change the brightness (no boot-time flash). */
	if(g_osd_enable)
		osd_install();
	else
		log_msg("osd: disabled (osd_enable=0) - hook not installed, nothing drawn");

	/* First-run credit: shown once, for ~4s. The OSD timer only counts down while
	 * the screen is actually drawing (see osd_tick), so firing it here gives the
	 * full visible window even though the XMB hasn't started rendering yet. */
	if(g_first_run && g_osd_enable)
		osd_message("BetterBright " BB_VERSION " by hobbo91");

	ApplySaved();    /* immediate attempt (in case we run after the firmware) */
	StartWorker();   /* load-window persistence + hold + save + combo_mode 2  */

	return 0;
}

int module_stop(SceSize args, void *argp)
{
	(void)args; (void)argp;
	g_running = 0;
	sceKernelDelayThread(150000);   /* let the worker (80ms loop) exit before we free */

	/* Flush a still-pending change so a clean unload (e.g. exiting a game) keeps
	 * it, even if the ~1s settle hadn't elapsed yet. */
	if(g_dirty && (saved_level != g_disk_level || cur != g_disk_index))
		SaveBrightness(data_path, saved_level, cur);

	memoryFree(bright_buf);
	return 0;
}
