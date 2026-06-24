/*
 * BetterBright  -  brightness control plugin for PSP (CFW: ARK-4 / FasterARK)
 *
 *   v0.1   -   developed by hobbo91  (https://github.com/hobbo91)
 *
 * Loosely based on the older "bright3" brightness plugin by hiroi01 (itself a
 * mod of "bright" by plum): https://hiroi01.com/?p=prx#bright3 - the original
 * screen-patching idea comes from there. BetterBright is otherwise a rework:
 * persistence, sleep/idle handling, two input schemes, and a modern toolchain.
 *
 * Features:
 *   - Press the Display/brightness button to cycle your configured values.
 *   - Remembers your level and re-applies it after XMB return, game launch,
 *     reboot, and resume from sleep.
 *   - Optional idle-dim hold and two optional adjust schemes (combo_mode).
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
int  g_hold_brightness = 1;  /* from ini: 1 = keep screen fully on (no dim, no auto-off) */
volatile int g_running = 1;  /* worker thread run flag                          */
volatile int g_dirty   = 0;  /* "brightness changed, please persist" flag       */

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
	g_dirty     = 1;                 /* ask WorkerThread to persist */
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
	if(n != cur) ApplyIndex(n);          /* only if it actually moves */
}
static void StepBackwardClamp(void)
{
	int n = (cur < 0) ? 0 : (cur - 1);
	if(n < 0) n = 0;
	if(n != cur) ApplyIndex(n);
}

/* Re-apply the remembered brightness WITHOUT advancing or re-saving. */
static void ApplySaved(void)
{
	if(saved_level >= 0) sceDisplaySetBrightness(saved_level, 0);
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
void sceDisplaySetBrightnessPatched(int level, int unk1)
{
	(void)level; (void)unk1;

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
			if(l && !r) { StepBackwardClamp(); return; }   /* L + Display = dimmer  */
			if(r && !l) { StepForwardClamp();  return; }   /* R + Display = brighter */
		}
	}

	StepForward();   /* plain Display button (or both/neither held) = cycle */
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
 * hold_brightness: the idle dim is NOT visible to GetBrightness (confirmed on
 * hardware), so it cannot be caught and undone. The only way to stop it is to
 * reset the display idle timer with scePowerTick. Because the dim and the
 * backlight auto-off are two stages of that SAME timer, this also keeps the
 * screen from turning off - one timer, two stages, they cannot be separated.
 * So hold_brightness = 1 means "screen stays fully on (no dim, no auto-off)".
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
	SceCtrlData pad;

	(void)args; (void)argp;

	g_load_ticks = LOAD_TICKS;

	while(g_running)
	{
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

		/* (b) hold_brightness: reset the display idle timer so the screen never
		 * dims. This unavoidably also stops the backlight auto-off (same timer) -
		 * i.e. the screen stays fully on. Set hold_brightness=0 for normal
		 * dimming + auto-off. */
		if(g_hold_brightness)
			scePowerTick(PSP_POWER_TICK_DISPLAY);

		/* (c) persist a pending change from a safe context */
		if(g_dirty)
		{
			g_dirty = 0;
			SaveBrightness(data_path, saved_level, cur);
		}

		/* (d) combo_mode 2 polling (L+R+Up/Down). combo_mode 1 is read in the hook. */
		if(g_combo_mode == 2)
		{
			if(sceCtrlPeekBufferPositive(&pad, 1) > 0)
			{
				int up_now   = ((pad.Buttons & COMBO_UP)   == COMBO_UP);
				int down_now = ((pad.Buttons & COMBO_DOWN) == COMBO_DOWN);

				/* rising edge only -> one step per press, clamped at the ends */
				if(up_now   && !up_prev)   StepForwardClamp();  /* L+R+Up   = brighter */
				if(down_now && !down_prev) StepBackwardClamp(); /* L+R+Down = dimmer  */

				up_prev   = up_now;
				down_prev = down_now;
			}
		}

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

	/* Build the BetterBright.ini path and read the configured values + settings. */
	strcpy(path, (char *)argp);
	GetConfigPath(path);

	bright_count = CountItem(path);
	if(bright_count <= 0) return -1;

	bright_buf = (Bright *)memoryAllocEx("ms_malloc", MEMORY_KERN_HI, 0,
	                                     sizeof(Bright) * bright_count, PSP_SMEM_Low, NULL);
	if(!bright_buf) return -1;

	/* ReadItem fills bright_buf and the ini settings. */
	{
		BrightSettings settings = { 0, 1 };   /* combo off, hold on */
		ReadItem(path, bright_buf, &settings);
		g_combo_mode      = settings.combo_mode;
		g_hold_brightness = settings.hold_brightness;
	}

	/* Restore remembered brightness (if any) and resume the cycle just after it. */
	if(LoadBrightness(data_path, &saved_level, &saved_index) == 0)
	{
		if(saved_index < 0 || saved_index >= bright_count) saved_index = 0;
		cur = saved_index;
	}

	mod = (SceModule2 *)sceKernelFindModuleByName("sceDisplay_Service");
	if(mod == NULL) return -1;

	if(GetPatchAddr(mod->text_addr, mod->text_addr + mod->text_size, &patch_addr) == 0)
	{
		MAKE_CALL(patch_addr, sceDisplaySetBrightnessPatched);
		ClearCaches();
	}
	else
	{
		previous = sctrlHENSetStartModuleHandler(OnModuleStart);
	}

	ApplySaved();    /* immediate attempt (in case we run after the firmware) */
	StartWorker();   /* load-window persistence + hold + save + combo_mode 2  */

	return 0;
}

int module_stop(SceSize args, void *argp)
{
	(void)args; (void)argp;
	g_running = 0;
	sceKernelDelayThread(150000);   /* let the worker (80ms loop) exit before we free */
	memoryFree(bright_buf);
	return 0;
}
