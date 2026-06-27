/*
 * log.c  -  opt-in diagnostic log for BetterBright.
 *
 * Ring-buffered: log_* calls format a line into an in-RAM ring (safe from any
 * context, including the brightness/display hooks); the worker thread calls
 * log_drain() to write pending lines to <plugin dir>/BetterBright.log from its
 * safe context. Self-contained: no stdio/sprintf (this is a -nostdlib kernel PRX),
 * just sceIo* with k1 cleared, exactly like SaveBrightness. See log.h.
 */

#include <pspkernel.h>
#include <pspsdk.h>      /* pspSdkSetK1 / pspSdkDisableInterrupts */
#include <string.h>
#include "log.h"

#ifndef PSP_O_APPEND
#define PSP_O_APPEND 0x0100
#endif

/* Ring geometry. RING_N lines of LOGLINE chars each (~8 KB of BSS). Big enough to
 * absorb a burst during the ~10 s load window before the worker first drains. */
#define RING_N   96
#define LOGLINE  80

static char g_path[256];
static int  g_on        = 0;
static int  g_have_path = 0;

static char         ring_text[RING_N][LOGLINE];
static unsigned int ring_time[RING_N];           /* push-time microseconds */
static volatile unsigned int ring_head = 0;      /* next slot to write     */
static volatile unsigned int ring_tail = 0;      /* next slot to drain     */
static volatile unsigned int ring_drops = 0;     /* overruns since reset    */

void log_set_path(const char *plugin_path)
{
	char *p;
	strcpy(g_path, plugin_path);
	p = strrchr(g_path, '/');
	if(p) strcpy(p + 1, "BetterBright.log");
	else  strcpy(g_path, "ms0:/seplugins/BetterBright.log");
	g_have_path = 1;
}

void log_enable(int on){ g_on = on; }
int  log_is_on(void)   { return g_on; }

/* ---- ring push (ANY context) ---------------------------------------------- */
/* Copy a finished line into the ring with a timestamp. Interrupts are off only
 * for the few instructions that claim a slot and bump head, so two pushers (a hook
 * and the worker) can't claim the same slot. Pushers ONLY ever touch head; the
 * drain ONLY ever touches tail - so there's no cross-thread write race on either
 * index. On overflow the drain (single reader) skips ahead; a pusher just keeps
 * writing, dropping the oldest not-yet-drained line. slot is always taken mod N,
 * so the array access is bounds-safe whatever the indices do. */
static void ring_push(const char *s)
{
	unsigned int slot;
	int i, intc;

	if(!g_on) return;

	intc = pspSdkDisableInterrupts();
	slot = ring_head % RING_N;
	for(i = 0; i < LOGLINE - 1 && s[i]; i++) ring_text[slot][i] = s[i];
	ring_text[slot][i] = 0;
	ring_time[slot] = sceKernelGetSystemTimeLow();
	ring_head++;
	pspSdkEnableInterrupts(intc);
}

/* ---- tiny formatters (no stdio) ------------------------------------------- */
static void put_str(char *b, int *pos, int max, const char *s)
{
	while(*s && *pos < max - 1) b[(*pos)++] = *s++;
}
static void put_dec(char *b, int *pos, int max, int v)
{
	char tmp[12];
	int n = 0;
	unsigned int u;
	if(v < 0) { if(*pos < max - 1) b[(*pos)++] = '-'; u = (unsigned int)(-v); }
	else      u = (unsigned int)v;
	if(u == 0) tmp[n++] = '0';
	while(u) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
	while(n && *pos < max - 1) b[(*pos)++] = tmp[--n];
}
static void put_dec3(char *b, int *pos, int max, unsigned int v)  /* zero-padded ms */
{
	if(*pos < max - 1) b[(*pos)++] = (char)('0' + (v / 100) % 10);
	if(*pos < max - 1) b[(*pos)++] = (char)('0' + (v / 10) % 10);
	if(*pos < max - 1) b[(*pos)++] = (char)('0' + v % 10);
}
static void put_hex(char *b, int *pos, int max, unsigned int v)
{
	static const char hx[] = "0123456789ABCDEF";
	int i;
	if(*pos < max - 1) b[(*pos)++] = '0';
	if(*pos < max - 1) b[(*pos)++] = 'x';
	for(i = 28; i >= 0; i -= 4)
		if(*pos < max - 1) b[(*pos)++] = hx[(v >> i) & 0xF];
}

/* ---- public push helpers (ANY context) ------------------------------------ */
void log_msg(const char *s){ if(g_on) ring_push(s); }

void log_kv(const char *label, int v)
{
	char line[LOGLINE]; int pos = 0;
	if(!g_on) return;
	put_str(line, &pos, LOGLINE, label);
	if(pos < LOGLINE - 1) line[pos++] = '=';
	put_dec(line, &pos, LOGLINE, v);
	line[pos] = 0;
	ring_push(line);
}

void log_kx(const char *label, unsigned int v)
{
	char line[LOGLINE]; int pos = 0;
	if(!g_on) return;
	put_str(line, &pos, LOGLINE, label);
	if(pos < LOGLINE - 1) line[pos++] = '=';
	put_hex(line, &pos, LOGLINE, v);
	line[pos] = 0;
	ring_push(line);
}

void log_ks(const char *label, const char *v)
{
	char line[LOGLINE]; int pos = 0;
	if(!g_on) return;
	put_str(line, &pos, LOGLINE, label);
	if(pos < LOGLINE - 1) line[pos++] = '=';
	put_str(line, &pos, LOGLINE, v ? v : "(null)");
	line[pos] = 0;
	ring_push(line);
}

void log_event(const char *event, int fwL, int plU, const char *draw)
{
	char line[LOGLINE]; int pos = 0;
	if(!g_on) return;
	put_str(line, &pos, LOGLINE, "event=");
	put_str(line, &pos, LOGLINE, event);
	put_str(line, &pos, LOGLINE, " fwL=");
	put_dec(line, &pos, LOGLINE, fwL);
	put_str(line, &pos, LOGLINE, " plU=");
	put_dec(line, &pos, LOGLINE, plU);
	if(draw) { put_str(line, &pos, LOGLINE, " draw="); put_str(line, &pos, LOGLINE, draw); }
	line[pos] = 0;
	ring_push(line);
}

/* ---- file side (SAFE CONTEXT ONLY) ---------------------------------------- */
void log_reset(void)
{
	SceUID fd;
	u32 k1;

	ring_head = ring_tail = ring_drops = 0;
	if(!g_have_path) return;

	k1 = pspSdkSetK1(0);
	fd = sceIoOpen(g_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if(fd >= 0) sceIoClose(fd);
	pspSdkSetK1(k1);
}

/* Flush every pending ring line to the file in one open/close. Snapshots the head
 * once so lines pushed mid-drain are simply caught next time. */
void log_drain(void)
{
	SceUID fd;
	u32 k1;
	unsigned int head;

	if(!g_on || !g_have_path) return;
	head = ring_head;                    /* snapshot (atomic int read) */
	if(ring_tail == head) return;        /* nothing pending */

	/* If pushers lapped us since last drain, skip ahead to the oldest line still
	 * in the ring (drain is the only writer of tail, so this is race-free). */
	if(head - ring_tail > RING_N)
	{
		ring_drops += (head - ring_tail) - RING_N;
		ring_tail = head - RING_N;
	}

	k1 = pspSdkSetK1(0);
	fd = sceIoOpen(g_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if(fd >= 0)
	{
		static unsigned int last_drops = 0;
		sceIoLseek(fd, 0, 2 /* PSP_SEEK_END */);   /* force EOF in case O_APPEND is flaky */

		if(ring_drops != last_drops)               /* note any lines lost to a burst */
		{
			char d[48]; int p = 0;
			put_str(d, &p, (int)sizeof(d), "*** log overflow: dropped ");
			put_dec(d, &p, (int)sizeof(d), (int)(ring_drops - last_drops));
			put_str(d, &p, (int)sizeof(d), " line(s) ***");
			sceIoWrite(fd, d, p);
			sceIoWrite(fd, "\r\n", 2);
			last_drops = ring_drops;
		}

		while(ring_tail != head)
		{
			unsigned int slot = ring_tail % RING_N;
			unsigned int us   = ring_time[slot];
			char line[LOGLINE + 24];
			int pos = 0;

			/* "[<sec>.<ms>] " timestamp prefix */
			line[pos++] = '[';
			put_dec(line, &pos, (int)sizeof(line), (int)(us / 1000000u));
			line[pos++] = '.';
			put_dec3(line, &pos, (int)sizeof(line), (us / 1000u) % 1000u);
			line[pos++] = ']'; line[pos++] = ' ';
			put_str(line, &pos, (int)sizeof(line), ring_text[slot]);

			sceIoWrite(fd, line, pos);
			sceIoWrite(fd, "\r\n", 2);
			ring_tail++;
		}
		sceIoClose(fd);
	}
	pspSdkSetK1(k1);
}
