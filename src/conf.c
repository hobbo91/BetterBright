/* config + persistence */

#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspsdk.h>
#include <string.h>
#include "conf.h"

/* Minimal integer parser, so we don't pull in newlib's atoi()/stdlib (which in
 * turn drags stdio + locking stubs into a kernel PRX). Values here are small
 * (brightness 0-100, option flags), so this is all we need. */
static int str_to_int(const char *s)
{
	int v = 0, neg = 0;
	if(*s == '-') { neg = 1; s++; }
	else if(*s == '+') { s++; }
	while(*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
	return neg ? -v : v;
}

/* dim_level= parsing. "AUTO" (any case) -> -1 (use 2nd-lowest ini value at apply
 * time). A plain number in 0-100 is used as-is. Anything else (out of range,
 * non-numeric) falls back to 28, per spec. */
static int parse_dim_level(const char *v)
{
	if((v[0]=='A'||v[0]=='a') && (v[1]=='U'||v[1]=='u') &&
	   (v[2]=='T'||v[2]=='t') && (v[3]=='O'||v[3]=='o'))
		return -1;
	if(v[0] >= '0' && v[0] <= '9')
	{
		int val = str_to_int(v);
		if(val >= 0 && val <= 100) return val;
	}
	return 28;
}

/* On-disk format for BetterBright.dat. The magic guards against reading a stale or
 * unrelated file written by an older/newer build. */
#define BRIGHT_MAGIC 0x42523304   /* 'B''R''3', version 4 */
typedef struct {
	int magic;
	int level;   /* 0-100      */
	int index;   /* index into the BetterBright.ini list */
} BrightSave;

void *malloc_p(SceSize size)
{
	SceUID memid;
	void *p;

	memid = sceKernelAllocPartitionMemory(2, "block_mem", 0, size, NULL);
	if(memid < 0) return NULL;

	p = (void *)sceKernelGetBlockHeadAddr(memid);
	return p;
}

int ReadLine(SceUID fd, char *buf, int n)
{
	int res, i = 0;
	char ch;

	do
	{
		res = sceIoRead(fd, &ch, 1);

		if(ch == '\r' || ch == ' ') continue;
		else if(ch == '\n') break;
		else buf[i++] = ch;
	}
	while(res > 0 && i < n);

	buf[i] = '\0';
	return i;
}

int Check_EOF(SceUID fd)
{
	char ch;

	if(sceIoRead(fd, &ch, 1) == 1)
	{
		sceIoLseek(fd, -1, PSP_SEEK_CUR);
		return 0;
	}
	return 1;
}

/* Replace the trailing file name of `buf` with "BetterBright.ini". */
int GetConfigPath(char *buf)
{
	char *p = strrchr(buf, '/');
	if(!p) return -1;
	strcpy(p + 1, "BetterBright.ini");
	return 0;
}

/* Replace the trailing file name of `buf` with "BetterBright.dat". */
int GetDataPath(char *buf)
{
	char *p = strrchr(buf, '/');
	if(!p) return -1;
	strcpy(p + 1, "BetterBright.dat");
	return 0;
}

/*
 * Returns 1 if `line` was a recognised "key=value" setting line (so it must NOT
 * be treated as a brightness value), or 0 if it is a plain brightness number.
 * Unknown key=value lines are still consumed (returns 1) so a typo'd option can
 * never be mistaken for a brightness level.
 */
static int parse_setting(const char *line, BrightSettings *s)
{
	const char *eq = strchr(line, '=');
	int klen, val;

	if(!eq) return 0;                 /* no '=' -> it's a brightness value */
	if(!s)  return 1;

	klen = (int)(eq - line);
	val  = str_to_int(eq + 1);

	if(klen == 10 && strncmp(line, "combo_mode", 10) == 0)
		s->combo_mode = val;
	else if(klen == 9 && strncmp(line, "dim_level", 9) == 0)
		s->dim_level = parse_dim_level(eq + 1);
	else if(klen == 15 && strncmp(line, "keep_display_on", 15) == 0)
		s->keep_display_on = val;
	else if(klen == 13 && strncmp(line, "disable_sleep", 13) == 0)
		s->disable_sleep = val;
	else if(klen == 10 && strncmp(line, "osd_enable", 10) == 0)
		s->osd_enable = val;
	else if(klen == 12 && strncmp(line, "debug_enable", 12) == 0)
		s->debug_enable = val;
	else if(klen == 13 && strncmp(line, "osd_bg_colour", 13) == 0)
		s->osd_bg_colour = val;
	else if(klen == 15 && strncmp(line, "osd_text_colour", 15) == 0)
		s->osd_text_colour = val;
	else if(klen == 8 && strncmp(line, "osd_size", 8) == 0)
		s->osd_size = val;
	else if(klen == 12 && strncmp(line, "osd_position", 12) == 0)
		s->osd_position = val;

	return 1;
}

int CountItem(char *file)
{
	int count = 0;
	char buffer[256];

	SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0777);
	if(fd < 0)
	{
		/* fall back to the ms0: copy if the given device failed */
		file[0] = 'm';
		file[1] = 's';

		fd = sceIoOpen(file, PSP_O_RDONLY, 0777);
		if(fd < 0) return -1;
	}

	while(Check_EOF(fd) == 0)
	{
		ReadLine(fd, buffer, 256);

		/* Count only brightness numbers: skip blanks, comments and settings. */
		if(buffer[0] != '\0' && buffer[0] != '#' && strchr(buffer, '=') == NULL)
			count++;
	}

	sceIoClose(fd);
	return count;
}

int ReadItem(const char *file, Bright *buf, BrightSettings *settings)
{
	int count = 0;
	char buffer[256];

	if(settings)                                /* defaults */
	{
		settings->combo_mode          = 0;
		settings->dim_level           = -1;   /* AUTO */
		settings->keep_display_on     = 0;
		settings->disable_sleep       = 0;
		settings->osd_enable          = 1;
		settings->debug_enable        = 0;
		settings->osd_bg_colour       = 1;    /* black */
		settings->osd_text_colour     = 2;    /* white */
		settings->osd_size            = 1;    /* normal */
		settings->osd_position        = 1;    /* bottom */
	}

	SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0777);
	if(fd < 0) return -1;

	while(Check_EOF(fd) == 0)
	{
		ReadLine(fd, buffer, 256);

		if(buffer[0] == '\0' || buffer[0] == '#') continue;   /* blank / comment */
		if(parse_setting(buffer, settings))        continue;   /* key=value       */

		buf[count++].level = str_to_int(buffer);                 /* brightness value */
	}

	sceIoClose(fd);
	return count;
}

/* Persist {level,index} to BetterBright.dat. Best-effort: failure is non-fatal.
 * k1 is cleared so the file access is allowed regardless of caller context. */
int SaveBrightness(const char *file, int level, int index)
{
	BrightSave s;
	SceUID fd;
	u32 k1;

	s.magic = BRIGHT_MAGIC;
	s.level = level;
	s.index = index;

	k1 = pspSdkSetK1(0);
	fd = sceIoOpen(file, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if(fd < 0) { pspSdkSetK1(k1); return -1; }
	sceIoWrite(fd, &s, sizeof(s));
	sceIoClose(fd);
	pspSdkSetK1(k1);
	return 0;
}

/* Read BetterBright.dat. Returns 0 and fills level/index on success, -1 otherwise. */
int LoadBrightness(const char *file, int *level, int *index)
{
	BrightSave s;
	int r;
	u32 k1;
	SceUID fd;

	k1 = pspSdkSetK1(0);
	fd = sceIoOpen(file, PSP_O_RDONLY, 0777);
	if(fd < 0) { pspSdkSetK1(k1); return -1; }

	r = sceIoRead(fd, &s, sizeof(s));
	sceIoClose(fd);
	pspSdkSetK1(k1);

	if(r != (int)sizeof(s) || s.magic != BRIGHT_MAGIC) return -1;
	if(s.level < 0 || s.level > 100) return -1;

	if(level) *level = s.level;
	if(index) *index = s.index;
	return 0;
}
