/*
 * BetterBright  -  brightness control plugin for PSP (CFW: ARK-4 / FasterARK)
 *
 *   v0.91  -   developed by hobbo91  (https://github.com/hobbo91)
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
#include <pspimpose_driver.h>    /* sceImposeGetParam, PSP_IMPOSE_LANGUAGE (read-only)  */
#include <string.h>

/* Forward-declared (rather than pulling in pspsysmem_kernel.h, which can clash with
 * the ARK headers) - resolved from libpspkernel at link time. Returns the PSP model
 * id: 0 = 1000, 1 = 2000, 2 = 3000, 3 = Go, ... Used for the empty-list stock steps. */
int sceKernelGetModel(void);

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
#include "version.h"

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
int  g_osd_draw_mode = 0;    /* from ini: 0=auto 1=hook-only 2=poll-only        */
int  g_sys_language  = 1;    /* system language index (read at boot; default EN) */
int  g_sync_fw_level = 0;    /* from ini: 1 = sync firmware (impose) level to ours */
volatile int g_impose_guard   = 0;  /* 1 while WE write the impose level (re-entrancy) */
volatile int g_impose_pending = -1; /* >=0: a USER brightness the worker should sync */
int  g_debug_enable  = 0;    /* from ini: 0=off, 1=DEBUG OSD line, 2=OSD + log file */
volatile int g_running = 1;  /* worker thread run flag                          */
volatile int g_dirty   = 0;  /* "brightness changed, please persist" flag       */

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

/* Fill lv[4] with this model's 4 stock backlight steps, used when the ini brightness
 * list is left empty. PSP-1000/2000 use a lower set than 3000/Go/Street (matches the
 * reference Brightness-Control plugin). */
static void StockLevels(int lv[4])
{
	int m = sceKernelGetModel();
	if(m == 0 || m == 1) { lv[0] = 36; lv[1] = 44; lv[2] = 56; lv[3] = 68; }  /* 1000/2000 */
	else                 { lv[0] = 44; lv[1] = 60; lv[2] = 72; lv[3] = 84; }  /* 3000/Go/...*/
}

/* ----------------------------------------------------------------------------
 * Firmware backlight level (impose) sync   [sync_fw_level, default off]
 *
 * Keeps the firmware's OWN backlight level - the 4 stock steps as the impose
 * "backlight brightness" param (0-3) - in step with our brightness, by rounding our
 * level UP to the nearest stock step (clamped to the top): U=90->84, U=50->60,
 * U=10->44. Our exact brightness is re-applied right after, so the visible value is
 * unchanged.
 *
 * STABILITY - why this no longer hangs at boot: the previous version wrote the
 * impose param from ApplySaved(), which runs at module_start, i.e. before the
 * display/impose subsystem has settled - that early WRITE is what hung the PSP. Now
 * we ONLY sync in response to a real user press (ApplyIndex), and even then defer
 * the write to the worker and only once the load window has closed. A user press
 * can't happen until well after boot, so no impose write ever occurs at boot. The
 * write itself is guarded against re-entering our brightness patch. No-op unless
 * sync_fw_level is enabled.
 * ------------------------------------------------------------------------- */

/* Index (0-3) of the smallest stock step >= u, clamped to the top (round up). */
static int CeilStockIdx(int u)
{
	int lv[4], i;
	StockLevels(lv);
	for(i = 0; i < 4; i++) if(lv[i] >= u) return i;
	return 3;
}

/* Request a firmware-level sync to brightness u. Cheap/safe from any context - just
 * records it; the worker does the actual write. No-op unless sync_fw_level is on.
 * Called ONLY from a user brightness change, never from boot/wake/restore. */
static void SyncImpose(int u)
{
	if(g_sync_fw_level && u >= 0) g_impose_pending = u;
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
	SyncImpose(saved_level);                    /* request FW-level sync (if enabled) */
	if(g_osd_enable) osd_notify(saved_level);   /* show "Display Brightness: NN" */
	log_kv("apply.index", cur);
	log_kv("apply.level", saved_level);         /* brightness we just set         */
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
	if(n != cur) ApplyIndex(n);                         /* moved -> apply (+OSD +sync) */
	else                                                /* already at max: */
	{
		SyncImpose(saved_level);                        /* re-pin FW level (it drifts each press) */
		if(g_osd_enable) osd_notify(saved_level);       /* still show OSD */
	}
}
static void StepBackwardClamp(void)
{
	int n = (cur < 0) ? 0 : (cur - 1);
	if(n < 0) n = 0;
	if(n != cur) ApplyIndex(n);
	else                                                /* already at min: */
	{
		SyncImpose(saved_level);
		if(g_osd_enable) osd_notify(saved_level);
	}
}

/* Re-apply the remembered brightness WITHOUT advancing or re-saving. */
static void ApplySaved(void)
{
	if(saved_level >= 0)
	{
		sceDisplaySetBrightness(saved_level, 0);
		log_kv("restore.level", saved_level);   /* reasserted (wake/resume/load)  */
	}
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
	if(dim < saved_level) { sceDisplaySetBrightness(dim, 0); log_kv("dim.level", dim); }
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
static volatile unsigned int g_last_press_us = 0; /* time of the last GENUINE press  */
static volatile int g_dimmed = 0;                 /* 1 = we've applied our own dim   */
static volatile unsigned int g_last_wake_us = 0;  /* guards against re-dim on wake   */

#define WRAP_RECENT_US 5000000u   /* 5s: a real wrap follows a PRESS within this;
                                     a firmware idle-reset comes minutes later.
                                     Measured from the last genuine press (NOT the
                                     last patch call) so firmware re-asserts - which
                                     happen when brightness is above the FW default -
                                     can't make an idle-dim look like a recent wrap */
#define REDIM_GUARD_US 8000000u   /* don't re-dim within 8s of a wake (the wake's own
                                     firmware re-assert must not look like new idle) */
#define ANY_BTN (PSP_CTRL_SELECT|PSP_CTRL_START|PSP_CTRL_UP|PSP_CTRL_RIGHT|PSP_CTRL_DOWN| \
                 PSP_CTRL_LEFT|PSP_CTRL_LTRIGGER|PSP_CTRL_RTRIGGER|PSP_CTRL_TRIANGLE| \
                 PSP_CTRL_CIRCLE|PSP_CTRL_CROSS|PSP_CTRL_SQUARE)

/* One brightness-handler event -> the DEBUG OSD line (debug>=1) AND the log ring
 * (debug>=2; log_event self-gates). Both are safe from this hook context: the OSD
 * is pure memory, the log only pushes to RAM. level = firmware/native step, our
 * saved_level = the brightness we hold, plus which draw path is on screen. */
static void DbgEvent(const char *ev, int level)
{
	if(g_debug_enable >= 1 && g_osd_enable) osd_debug(ev, level, (unsigned int)saved_level);
	log_event(ev, level, saved_level, osd_draw_path_name());
}

void sceDisplaySetBrightnessPatched(int level, int unk1)
{
	unsigned int now = sceKernelGetSystemTimeLow();
	int prev = g_last_native;
	int press, did_combo = 0;
	(void)unk1;
	g_last_native   = level;

	/* This call was triggered by OUR own impose write (sync_fw_level), not a press
	 * or firmware event - update the baseline and bail. Only ever set when
	 * sync_fw_level is on, so inert by default. */
	if(g_impose_guard) return;

	/* A real Display press steps the native level UP (44->60->72->84) or wraps
	 * 84->44. The firmware off/idle/wake instead resets DOWN to 44 or repeats the
	 * level. The only collision is 84->44 - a genuine wrap arrives moments after
	 * the previous PRESS, a firmware reset arrives long after one. */
	if(prev < 0)                       press = 1;                      /* boot -> press */
	else if(level == prev)             press = 0;                      /* repeat -> firmware */
	else if(prev == 84 && level == 44)                                 /* wrap iff a press was recent */
		press = ((now - g_last_press_us) < WRAP_RECENT_US);
	else                               press = (level > prev);         /* up=press, down=firmware */

	if(press) g_last_press_us = now;   /* record the press time for the next wrap test */

	if(!press)
	{
		/* Firmware idle event. Apply OUR dim once - unless the display is held on,
		 * or we're within a few seconds of a wake (which re-asserts through here). */
		if(!g_keep_display_on && !g_dimmed && (now - g_last_wake_us) > REDIM_GUARD_US)
		{
			ApplyDim();
			g_dimmed = 1;
			DbgEvent("dim", level);
		}
		else DbgEvent("idle", level);
		return;
	}

	if(g_dimmed)
	{
		/* A press while dimmed = waking via the Display button -> restore, no cycle. */
		g_dimmed = 0;
		g_last_wake_us = now;
		ApplySaved();
		DbgEvent("wake", level);
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

	DbgEvent(did_combo ? "combo" : "press", level);
}

/* Re-assert our saved level for a short window right after the plugin loads, so a
 * fresh boot / game launch keeps the brightness you last set (the firmware sets
 * its own level during init; this puts yours back). */
#define LOAD_TICKS 125            /* 125 * 80ms ~= 10s */
static volatile int g_load_ticks = 0;

/* Worker-only: perform a pending firmware-level sync, if any. Writes the impose
 * backlight step (rounded UP from our level), guarded against patch re-entrancy,
 * then RE-APPLIES our exact brightness (the impose write coarsens the panel to the
 * stock step, so we restore the fine value back-to-back). Held off entirely until
 * the load window has closed, so it never fires near boot. */
static void ServiceImpose(void)
{
	int idx;
	u32 k1;

	if(g_impose_pending < 0) return;           /* nothing requested */
	if(!g_sync_fw_level || g_load_ticks > 0 || saved_level < 0) return;  /* not yet / off */
	g_impose_pending = -1;

	idx = CeilStockIdx(saved_level);

	k1 = pspSdkSetK1(0);
	g_impose_guard = 1;
	sceImposeSetParam(PSP_IMPOSE_BACKLIGHT_BRIGHTNESS, idx);
	g_impose_guard = 0;
	sceDisplaySetBrightness(saved_level, 0);   /* restore our exact value (bypasses the patch) */
	pspSdkSetK1(k1);

	{
		int lv[4];
		StockLevels(lv);
		g_last_native = lv[idx];                /* keep press baseline in sync */
		log_kv("impose.idx", idx);
		DbgEvent("imposed", lv[idx]);           /* visible test: L=<stock step> U=<level> */
	}
}

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
	const char *last_path = (const char *)0;   /* last logged OSD draw path */

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
				log_msg("resume-from-sleep: reasserting brightness");
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
			log_msg("wake (button): restoring brightness");
			ApplySaved();
			g_dimmed = 0;
			g_last_wake_us = sceKernelGetSystemTimeLow();
		}

		/* (a0) firmware-level sync (sync_fw_level): apply a pending impose write from
		 * this clean context. Self-gates to OFF and to load_ticks==0, so it never
		 * fires near boot - this is what fixes the old boot hang. */
		ServiceImpose();

		/* (a) load-window persistence: re-apply our level for ~10s after load if
		 * the firmware's init lowered it. (now > 0 just skips a transient 0.) */
		if(g_load_ticks > 0)
		{
			if(saved_level >= 0)
			{
				int now = -1, unk = 0;
				sceDisplayGetBrightness(&now, &unk);
				if(now > 0 && now < saved_level)
				{
					sceDisplaySetBrightness(saved_level, 0);
					log_kv("loadwin.reassert", saved_level);  /* undid firmware init dim */
				}
			}
			g_load_ticks--;
			if(g_load_ticks == 0) log_msg("load window ended");
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
					int r = SaveBrightness(data_path, saved_level, cur);
					g_disk_level = saved_level;
					g_disk_index = cur;
					log_msg("dat.write (brightness change)");
					log_kv("dat.write.level",  saved_level);
					log_kv("dat.write.index",  cur);
					log_kv("dat.write.result", r);   /* 0 = ok */
				}
			}
		}

		/* (d) tick down the on-screen-display visibility timer (only relevant
		 * when the OSD is enabled - when it's off, leave the subsystem alone) */
		if(g_osd_enable)
			osd_tick();

		/* (d2) note when the OSD's effective draw path flips (e.g. entering a game
		 * that doesn't drive the hook -> fb-poll). Pointer compare is fine: the
		 * names are static strings. Only meaningful when the log is on. */
		if(g_osd_enable && log_is_on())
		{
			const char *path = osd_draw_path_name();
			if(path != last_path) { log_ks("draw-path", path); last_path = path; }
		}

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

		/* (f) flush any log lines pushed since last loop (by the worker OR the
		 * hooks) to BetterBright.log. This is the ONLY place file I/O for the log
		 * happens - a safe context - which is what lets hooks log via the ring. */
		log_drain();

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

	/* Count VALID brightness values in the ini. <=0 means none were given (or the
	 * file is missing) - that's allowed: we fall back to the 4 model stock steps.
	 * Reserve room for 4 so the fallback always fits. */
	bright_count = CountItem(path);
	{
		int want = (bright_count > 0) ? bright_count : 4;
		bright_buf = (Bright *)memoryAllocEx("ms_malloc", MEMORY_KERN_HI, 0,
		                                     sizeof(Bright) * want, PSP_SMEM_Low, NULL);
	}
	if(!bright_buf) return -1;

	/* ReadItem fills bright_buf with the valid values and sets every settings field
	 * (defaults first), returning how many valid values it stored. */
	{
		BrightSettings settings;
		int n = ReadItem(path, bright_buf, &settings);
		g_combo_mode       = settings.combo_mode;
		g_dim_level        = settings.dim_level;
		g_keep_display_on  = settings.keep_display_on;
		g_disable_sleep    = settings.disable_sleep;
		g_osd_enable       = settings.osd_enable;
		g_debug_enable     = settings.debug_enable;
		g_sync_fw_level    = settings.sync_fw_level;
		osd_set_style(settings.osd_text_colour, settings.osd_bg_colour,
		              settings.osd_size, settings.osd_position);
		osd_set_draw_mode(settings.osd_draw_mode);
		g_osd_draw_mode = settings.osd_draw_mode;

		/* Localise the OSD "Brightness" word to the system language when
		 * detect_locale=1; otherwise force English. (Read-only impose param; k1
		 * cleared as this is a user-facing kernel call.) */
		if(settings.detect_locale)
		{
			u32 k1 = pspSdkSetK1(0);
			g_sys_language = sceImposeGetParam(PSP_IMPOSE_LANGUAGE);
			pspSdkSetK1(k1);
		}
		else
			g_sys_language = 1;   /* English */
		osd_set_language(g_sys_language);

		if(n > 0)
			bright_count = n;
		else
		{
			/* No usable values in the ini -> cycle the 4 stock steps for this model. */
			int lv[4], i;
			StockLevels(lv);
			for(i = 0; i < 4; i++) bright_buf[i].level = lv[i];
			bright_count = 4;
		}
	}

	/* Bring up logging as early as we can (the ini is the first thing we know).
	 * All log writes happen here in module_start or in the worker - never in the
	 * display/brightness hooks. */
	log_set_path((char *)argp);
	log_enable(g_debug_enable >= 2);   /* file log only at debug_enable=2          */
	if(g_debug_enable >= 2)
	{
		log_reset();                    /* fresh file each boot                     */
		log_msg("=== BetterBright v" BB_VERSION " module_start ===");
		log_ks("dat.path",                      data_path);
		log_kv("settings.combo_mode",           g_combo_mode);
		log_kv("settings.dim_level",            g_dim_level);
		log_kv("settings.keep_display_on",      g_keep_display_on);
		log_kv("settings.disable_sleep",        g_disable_sleep);
		log_kv("settings.osd_enable",           g_osd_enable);
		log_kv("settings.osd_draw_mode",        g_osd_draw_mode);
		log_kv("settings.debug_enable",         g_debug_enable);
		log_kv("settings.first_run",            g_first_run);
		log_kv("ini.bright_count",              bright_count);
		log_kv("model",                         sceKernelGetModel());
		log_kv("system.language",               g_sys_language);
		log_kv("settings.sync_fw_level",        g_sync_fw_level);
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
		{
			int r = SaveBrightness(data_path, saved_level, cur);  /* the .dat now exists */
			log_msg("firstrun: .dat created");
			log_kv("dat.write.result", r);
		}
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

	log_msg("module_start: done, worker starting");
	ApplySaved();    /* immediate attempt (in case we run after the firmware) */
	StartWorker();   /* load-window persistence + hold + save + combo_mode 2  */

	log_drain();     /* flush the boot lines now (don't wait for the first loop) */
	return 0;
}

int module_stop(SceSize args, void *argp)
{
	(void)args; (void)argp;
	log_msg("module_stop: unloading");
	g_running = 0;
	if(g_osd_enable) osd_shutdown();   /* stop the poll-draw thread before we free */
	sceKernelDelayThread(150000);   /* let the worker (80ms loop) exit before we free */

	/* Flush a still-pending change so a clean unload (e.g. exiting a game) keeps
	 * it, even if the ~1s settle hadn't elapsed yet. */
	if(g_dirty && (saved_level != g_disk_level || cur != g_disk_index))
	{
		int r = SaveBrightness(data_path, saved_level, cur);
		log_msg("dat.write (final flush on stop)");
		log_kv("dat.write.level",  saved_level);
		log_kv("dat.write.result", r);
	}

	/* Worker has exited (delay above), so we own the ring now - final flush. */
	log_drain();
	memoryFree(bright_buf);
	return 0;
}
