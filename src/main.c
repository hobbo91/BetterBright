/*
 * BetterBright - PSP brightness plugin (ARK-4 / FasterARK).
 * v0.91 by hobbo91 - https://github.com/hobbo91
 * Based on hiroi01's "bright3" (a mod of plum's "bright").
 */

#include <pspkernel.h>
#include <pspdisplay_kernel.h>   /* sceDisplaySetBrightness / sceDisplayGetBrightness */
#include <pspctrl.h>             /* sceCtrlPeekBufferPositive, PSP_CTRL_* , SceCtrlData */
#include <psppower.h>            /* scePowerTick, PSP_POWER_TICK_DISPLAY               */
#include <pspsdk.h>              /* pspSdkSetK1 - clear syscall guard around the ctrl read */
#include <pspimpose_driver.h>    /* sceImposeGetParam, PSP_IMPOSE_LANGUAGE (read-only)  */
#include <string.h>

/* Forward-declared to avoid pspsysmem_kernel.h clashing with ARK headers; linked
 * from libpspkernel. Model id: 0=1000 1=2000 2=3000 3=Go ... (empty-list steps). */
int sceKernelGetModel(void);

/* Only SceModule2 + sctrlHENSetStartModuleHandler are needed; <systemctrl.h> clashes
 * with current pspdev headers, so pull in just the struct and forward-declare. */
#include <module2.h>
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

/* Write-coalescing: a burst of presses becomes one .dat write once changes settle.
 * g_disk_* mirror what's on disk so we never rewrite the same value. */
#define SAVE_SETTLE 12              /* 12 * 80ms ~= 1s of no change before writing */
volatile int g_save_settle = 0;
int g_disk_level = -2;              /* -2 = unknown (no .dat read yet)             */
int g_disk_index = -2;

/* ---- patch helpers ---- */
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

/* This model's 4 stock backlight steps (used when the ini list is empty). */
static void StockLevels(int lv[4])
{
	int m = sceKernelGetModel();
	if(m == 0 || m == 1) { lv[0] = 36; lv[1] = 44; lv[2] = 56; lv[3] = 68; }  /* 1000/2000 */
	else                 { lv[0] = 44; lv[1] = 60; lv[2] = 72; lv[3] = 84; }  /* 3000/Go/...*/
}

/* ---- firmware backlight level (impose) sync  [sync_fw_level] ----
 * Pins the firmware's stock-step level (impose param 0-3) to our brightness,
 * rounded up. The actual write is deferred to the worker and only after the load
 * window - never at boot (an early impose write used to hang the PSP). No-op when
 * sync_fw_level is off. */

/* Index (0-3) of the smallest stock step >= u, clamped to the top (round up). */
static int CeilStockIdx(int u)
{
	int lv[4], i;
	StockLevels(lv);
	for(i = 0; i < 4; i++) if(lv[i] >= u) return i;
	return 3;
}

/* Request a sync (worker does the write). Only from a user change, never boot/wake. */
static void SyncImpose(int u)
{
	if(g_sync_fw_level && u >= 0) g_impose_pending = u;
}

/* ---- brightness application + persistence ---- */

/* Apply bright_buf[i]; the .dat write is deferred to the worker (file I/O is unsafe
 * in the display-hook context this can run from). */
static void ApplyIndex(int i)
{
	if(i < 0 || i >= bright_count) return;
	cur         = i;
	saved_level = bright_buf[i].level;
	sceDisplaySetBrightness(saved_level, 0);
	SyncImpose(saved_level);
	if(g_osd_enable) osd_notify(saved_level);
	log_kv("apply.index", cur);
	log_kv("apply.level", saved_level);
	g_dirty       = 1;                 /* persist (deferred, debounced) */
	g_save_settle = SAVE_SETTLE;
}

/* Display button: step forward with wrap. */
static void StepForward(void)
{
	int n = (cur < 0) ? 0 : (cur + 1) % bright_count;
	ApplyIndex(n);
}

/* Combo: step toward an end and stop there (no wrap). At the end, still re-pin the
 * FW level (it drifts each press) and show the OSD. */
static void StepForwardClamp(void)
{
	int n = (cur < 0) ? 0 : (cur + 1);
	if(n > bright_count - 1) n = bright_count - 1;
	if(n != cur) ApplyIndex(n);
	else { SyncImpose(saved_level); if(g_osd_enable) osd_notify(saved_level); }
}
static void StepBackwardClamp(void)
{
	int n = (cur < 0) ? 0 : (cur - 1);
	if(n < 0) n = 0;
	if(n != cur) ApplyIndex(n);
	else { SyncImpose(saved_level); if(g_osd_enable) osd_notify(saved_level); }
}

/* Re-apply the remembered brightness (wake/resume/load), no advance/save. */
static void ApplySaved(void)
{
	if(saved_level >= 0)
	{
		sceDisplaySetBrightness(saved_level, 0);
		log_kv("restore.level", saved_level);
	}
}

/* Drop to the dim level (AUTO = 2nd-lowest) without touching saved_level; never
 * brightens. */
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

/* ---- Display-button hook ----
 * The firmware steps its native level (44/60/72/84) on every real press and passes
 * it here. We classify press vs firmware event by the level's direction, then cycle
 * (plain press) or adjust (combo_mode 1: L/R + Display = dimmer/brighter, clamped). */
static volatile int g_last_native = -1;
static volatile unsigned int g_last_press_us = 0; /* time of the last genuine press  */
static volatile int g_dimmed = 0;                 /* 1 = we've applied our own dim   */
static volatile unsigned int g_last_wake_us = 0;  /* guards against re-dim on wake   */

#define WRAP_RECENT_US 5000000u   /* 84->44 is a wrap iff within 5s of a real press
                                     (vs an idle-reset minutes later) */
#define REDIM_GUARD_US 8000000u   /* don't re-dim within 8s of a wake */
#define ANY_BTN (PSP_CTRL_SELECT|PSP_CTRL_START|PSP_CTRL_UP|PSP_CTRL_RIGHT|PSP_CTRL_DOWN| \
                 PSP_CTRL_LEFT|PSP_CTRL_LTRIGGER|PSP_CTRL_RTRIGGER|PSP_CTRL_TRIANGLE| \
                 PSP_CTRL_CIRCLE|PSP_CTRL_CROSS|PSP_CTRL_SQUARE)

/* One event -> DEBUG OSD line (debug>=1) + log ring (debug>=2). Safe in this hook:
 * pure-memory OSD, ring-only log. level = firmware step, saved_level = our level. */
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

	if(g_impose_guard) return;   /* our own impose write - ignore (inert unless sync on) */

	/* Up (or recent 84->44 wrap) = press; down/repeat = firmware idle/wake. */
	if(prev < 0)                       press = 1;
	else if(level == prev)             press = 0;
	else if(prev == 84 && level == 44) press = ((now - g_last_press_us) < WRAP_RECENT_US);
	else                               press = (level > prev);

	if(press) g_last_press_us = now;

	if(!press)
	{
		/* Firmware idle: dim once (unless held on, or just woke). */
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
		/* press while dimmed = wake via Display -> restore, don't cycle */
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
			if(l && !r)      { StepBackwardClamp(); did_combo = 1; }   /* L = dimmer  */
			else if(r && !l) { StepForwardClamp();  did_combo = 1; }   /* R = brighter */
		}
	}
	if(!did_combo) StepForward();

	DbgEvent(did_combo ? "combo" : "press", level);
}

/* Re-assert our level for ~10s after load (the firmware sets its own during init). */
#define LOAD_TICKS 125            /* 125 * 80ms ~= 10s */
static volatile int g_load_ticks = 0;

/* Worker-only: do a pending impose sync - write the stock step (guarded), then
 * re-apply our exact brightness. Held off until the load window closes (not at boot). */
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
		DbgEvent("imposed", lv[idx]);
	}
}

/* ---- worker thread ----
 * Handles: load-window persistence (re-assert our level ~10s after load), the
 * keep_display_on idle-timer tick, resume/wake re-assert, the impose sync, .dat
 * persistence, and combo_mode 2 polling. (combo_mode 1 is in the hook.) The idle
 * dim and wake-from-sleep levels aren't visible to GetBrightness - see README. */
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

		/* (0) resume-from-sleep: a multi-second loop gap = was suspended; re-assert. */
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

		/* (0b) wake from our dim via any button (Display wakes via the patch). */
		if(g_dimmed && have_pad && (pad.Buttons & ANY_BTN))
		{
			log_msg("wake (button): restoring brightness");
			ApplySaved();
			g_dimmed = 0;
			g_last_wake_us = sceKernelGetSystemTimeLow();
		}

		/* (a0) firmware-level sync (self-gates off / to after the load window). */
		ServiceImpose();

		/* (a) load-window persistence: undo the firmware's init dim for ~10s. */
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

		/* (b) keep_display_on: tick the display idle timer (one timer = dim + off). */
		if(g_keep_display_on)
			scePowerTick(PSP_POWER_TICK_DISPLAY);

		/* disable_sleep: tick the auto-sleep timer (manual sleep still works). */
		if(g_disable_sleep)
			scePowerTick(PSP_POWER_TICK_SUSPEND);

		/* (c) persist a settled change if it differs from disk. */
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

		/* (d) tick the OSD visibility timer. */
		if(g_osd_enable)
			osd_tick();

		/* (d2) log when the OSD draw path flips (static-string pointer compare). */
		if(g_osd_enable && log_is_on())
		{
			const char *path = osd_draw_path_name();
			if(path != last_path) { log_ks("draw-path", path); last_path = path; }
		}

		/* (e) combo_mode 2: L+R+Up/Down, rising-edge, clamped. */
		if(g_combo_mode == 2 && have_pad)
		{
			int up_now   = ((pad.Buttons & COMBO_UP)   == COMBO_UP);
			int down_now = ((pad.Buttons & COMBO_DOWN) == COMBO_DOWN);

			if(up_now   && !up_prev)   StepForwardClamp();  /* brighter */
			if(down_now && !down_prev) StepBackwardClamp(); /* dimmer  */

			up_prev   = up_now;
			down_prev = down_now;
		}

		/* (f) flush the log ring to disk (only safe place for the file I/O). */
		log_drain();

		/* ~80ms loop; during the load window split it to undo the init dim faster. */
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

/* ---- module hooks ---- */
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

	/* Derive the .dat path before path[] gets reused for the .ini. */
	strcpy(data_path, (char *)argp);
	GetDataPath(data_path);

	/* First run = no .dat yet (we show a one-off credit when it's created). */
	{
		u32 k1 = pspSdkSetK1(0);
		SceUID fd = sceIoOpen(data_path, PSP_O_RDONLY, 0777);
		if(fd >= 0) { sceIoClose(fd); g_first_run = 0; }
		else          g_first_run = 1;
		pspSdkSetK1(k1);
	}

	/* Read the .ini values + settings. */
	strcpy(path, (char *)argp);
	GetConfigPath(path);

	/* Count valid values; <=0 (none/empty) falls back to 4 stock steps, so reserve 4. */
	bright_count = CountItem(path);
	{
		int want = (bright_count > 0) ? bright_count : 4;
		bright_buf = (Bright *)memoryAllocEx("ms_malloc", MEMORY_KERN_HI, 0,
		                                     sizeof(Bright) * want, PSP_SMEM_Low, NULL);
	}
	if(!bright_buf) return -1;

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

		/* OSD word language: system language if detect_locale, else English. */
		if(settings.detect_locale)
		{
			u32 k1 = pspSdkSetK1(0);
			g_sys_language = sceImposeGetParam(PSP_IMPOSE_LANGUAGE);
			pspSdkSetK1(k1);
		}
		else
			g_sys_language = 1;
		osd_set_language(g_sys_language);

		if(n > 0)
			bright_count = n;
		else
		{
			/* empty ini list -> cycle the 4 stock steps */
			int lv[4], i;
			StockLevels(lv);
			for(i = 0; i < 4; i++) bright_buf[i].level = lv[i];
			bright_count = 4;
		}
	}

	/* Logging (file only at debug_enable=2). */
	log_set_path((char *)argp);
	log_enable(g_debug_enable >= 2);
	if(g_debug_enable >= 2)
	{
		log_reset();                    /* fresh file each boot */
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

	/* First run: create the .dat now, taking the current screen brightness (mapped to
	 * the nearest step) so nothing visibly changes. */
	if(g_first_run)
	{
		int lvl = -1, unk = 0, i, best = 0, bestd = 1000;
		u32 k1 = pspSdkSetK1(0);
		sceDisplayGetBrightness(&lvl, &unk);
		pspSdkSetK1(k1);
		if(lvl < 1 || lvl > 100) lvl = bright_buf[bright_count - 1].level; /* fallback: brightest */

		for(i = 0; i < bright_count; i++)             /* nearest step */
		{
			int d = bright_buf[i].level - lvl; if(d < 0) d = -d;
			if(d < bestd) { bestd = d; best = i; }
		}

		saved_level  = lvl;
		saved_index  = best;
		cur          = best;
		{
			int r = SaveBrightness(data_path, saved_level, cur);
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

	/* OSD hook only when enabled (otherwise no overhead at all). */
	if(g_osd_enable)
		osd_install();
	else
		log_msg("osd: disabled");

	/* First-run credit, shown once for ~4s. */
	if(g_first_run && g_osd_enable)
		osd_message("BetterBright " BB_VERSION " by hobbo91");

	log_msg("module_start: done, worker starting");
	ApplySaved();    /* immediate attempt (in case we run after the firmware) */
	StartWorker();

	log_drain();     /* flush boot lines now */
	return 0;
}

int module_stop(SceSize args, void *argp)
{
	(void)args; (void)argp;
	log_msg("module_stop: unloading");
	g_running = 0;
	if(g_osd_enable) osd_shutdown();   /* stop the poll-draw thread first */
	sceKernelDelayThread(150000);   /* let the worker exit before we free */

	/* Flush a still-pending change so a clean unload keeps it. */
	if(g_dirty && (saved_level != g_disk_level || cur != g_disk_index))
	{
		int r = SaveBrightness(data_path, saved_level, cur);
		log_msg("dat.write (final flush on stop)");
		log_kv("dat.write.level",  saved_level);
		log_kv("dat.write.result", r);
	}

	log_drain();     /* worker has exited - final flush */
	memoryFree(bright_buf);
	return 0;
}
