/*
 * log.c  -  opt-in diagnostic log for BetterBright.
 *
 * Appends plain text lines to <plugin dir>/BetterBright.log. Self-contained: no
 * stdio/sprintf (this is a -nostdlib kernel PRX), just sceIo* with k1 cleared,
 * exactly like SaveBrightness. Safe-context callers only (see log.h).
 */

#include <pspkernel.h>
#include <pspsdk.h>      /* pspSdkSetK1 */
#include <string.h>
#include "log.h"

#ifndef PSP_O_APPEND
#define PSP_O_APPEND 0x0100
#endif

static char g_path[256];
static int  g_on        = 0;
static int  g_have_path = 0;

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

static int slen(const char *s){ int n = 0; while(s[n]) n++; return n; }

/* append one CRLF-terminated line */
static void write_line(const char *s)
{
	SceUID fd;
	u32 k1;

	if(!g_on || !g_have_path) return;

	k1 = pspSdkSetK1(0);
	fd = sceIoOpen(g_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if(fd >= 0)
	{
		sceIoLseek(fd, 0, 2 /* PSP_SEEK_END */);   /* force EOF in case O_APPEND is flaky */
		sceIoWrite(fd, s, slen(s));
		sceIoWrite(fd, "\r\n", 2);
		sceIoClose(fd);
	}
	pspSdkSetK1(k1);
}

void log_reset(void)
{
	SceUID fd;
	u32 k1;

	if(!g_have_path) return;

	k1 = pspSdkSetK1(0);
	fd = sceIoOpen(g_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if(fd >= 0) sceIoClose(fd);
	pspSdkSetK1(k1);
}

/* ---- tiny formatters (no stdio) ------------------------------------------- */
static void put_str(char *b, int *pos, const char *s){ while(*s) b[(*pos)++] = *s++; }

static void put_dec(char *b, int *pos, int v)
{
	char tmp[12];
	int n = 0, neg = 0;
	unsigned int u;

	if(v < 0) { neg = 1; u = (unsigned int)(-v); } else u = (unsigned int)v;
	if(u == 0) tmp[n++] = '0';
	while(u) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
	if(neg) b[(*pos)++] = '-';
	while(n) b[(*pos)++] = tmp[--n];
}

static void put_hex(char *b, int *pos, unsigned int v)
{
	static const char hx[] = "0123456789ABCDEF";
	int i;
	b[(*pos)++] = '0';
	b[(*pos)++] = 'x';
	for(i = 28; i >= 0; i -= 4)
		b[(*pos)++] = hx[(v >> i) & 0xF];
}

void log_msg(const char *s){ if(g_on) write_line(s); }

void log_kv(const char *label, int v)
{
	char line[160];
	int pos = 0;
	if(!g_on) return;
	put_str(line, &pos, label);
	line[pos++] = '=';
	put_dec(line, &pos, v);
	line[pos] = 0;
	write_line(line);
}

void log_kx(const char *label, unsigned int v)
{
	char line[160];
	int pos = 0;
	if(!g_on) return;
	put_str(line, &pos, label);
	line[pos++] = '=';
	put_hex(line, &pos, v);
	line[pos] = 0;
	write_line(line);
}
