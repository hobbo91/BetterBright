/*
 * osd.c - on-screen "Brightness: NN" overlay.
 *
 * Drawn in two ways (osd_draw_mode): the sceDisplaySetFrameBuf hook (draws into the
 * frame the system is about to show), and a poll-draw fallback thread for games that
 * don't drive that hook. Pure memory writes (bitmap font into the framebuffer), no
 * syscalls in the draw path - safe inside the hook.
 */

#include <pspkernel.h>
#include <module2.h>         /* SceModule2 (CFW module layout)             */
#include <psploadcore.h>     /* SceLibraryEntryTable                       */
#include <pspsdk.h>          /* pspSdkDisableInterrupts / EnableInterrupts */
#include <string.h>
#include "osd.h"
#include "log.h"
#include "version.h"

/* Export tables must be walked via SceModule2 (not pspsdk's SceModule, whose ent_top
 * is 8 bytes off on CFW). */

/* ---- syscall-table hook ----
 * User-mode callers reach sceDisplaySetFrameBuf via the syscall table; redirecting
 * its slot catches them. cop0 $12 points at the running table. */
struct sce_syscall_header { void *unk; u32 basenum; u32 topnum; u32 size; };

/* Address of the syscall-table slot that dispatches to kernel function `fn`. */
static u32 *find_syscall_slot(u32 fn)
{
	void **ptr;
	struct sce_syscall_header *head;
	u32 *slots;
	int count, i;

	__asm__ volatile ("cfc0 %0, $12\n nop\n" : "=r"(ptr));
	if(!ptr) return 0;

	head  = (struct sce_syscall_header *)(*ptr);
	slots = (u32 *)((u32)*ptr + 0x10);
	count = ((int)head->size - 0x10) / (int)sizeof(u32);

	for(i = 0; i < count; i++)
		if(slots[i] == fn) return &slots[i];
	return 0;
}

/* Find an export address by module / library / NID. */
static u32 find_export(const char *modname, const char *lib, u32 nid)
{
	SceModule2 *mod = (SceModule2 *)sceKernelFindModuleByName(modname);
	int i = 0;
	if(!mod) return 0;
	while(i < (int)mod->ent_size)
	{
		SceLibraryEntryTable *e = (SceLibraryEntryTable *)((u32)mod->ent_top + i);
		if(e->libname && strcmp(e->libname, lib) == 0)
		{
			u32 *table = (u32 *)e->entrytable;
			int total = e->stubcount + e->vstubcount;
			int u;
			for(u = 0; u < total; u++)
				if(table[u] == nid) return table[u + total];
		}
		i += (e->len << 2);
	}
	return 0;
}

/* ---- instruction-hijack fallback ----
 * If the syscall-slot patch can't apply, redirect the function entry, saving the
 * first two instructions into a trampoline so the original stays callable. */
#define MAKE_JUMP(a, f)  _sw(0x08000000 | (((u32)(f) >> 2) & 0x03FFFFFF), (a))
static u32 g_tramp[3];

static void *hijack_install(u32 addr, void *newf)
{
	g_tramp[0] = _lw(addr + 0);                 /* original instr 0           */
	g_tramp[2] = _lw(addr + 4);                 /* original instr 1 (delay)   */
	MAKE_JUMP((u32)&g_tramp[1], addr + 8);      /* then jump back to addr+8   */
	_sw(0x08000000 | (((u32)newf >> 2) & 0x03FFFFFF), addr); /* j newf at addr*/
	_sw(0, addr + 4);                            /* nop in its delay slot      */
	return (void *)g_tramp;
}


/* 8x8 1bpp bitmap font (MSB = leftmost col, 8 bytes/glyph), the public-domain
 * pspsdk debug font embedded so we don't depend on -lpspdebug. */
static const u8 bb_font[] =
"\x00\x00\x00\x00\x00\x00\x00\x00\x3c\x42\xa5\x81\xa5\x99\x42\x3c"
"\x3c\x7e\xdb\xff\xff\xdb\x66\x3c\x6c\xfe\xfe\xfe\x7c\x38\x10\x00"
"\x10\x38\x7c\xfe\x7c\x38\x10\x00\x10\x38\x54\xfe\x54\x10\x38\x00"
"\x10\x38\x7c\xfe\xfe\x10\x38\x00\x00\x00\x00\x30\x30\x00\x00\x00"
"\xff\xff\xff\xe7\xe7\xff\xff\xff\x38\x44\x82\x82\x82\x44\x38\x00"
"\xc7\xbb\x7d\x7d\x7d\xbb\xc7\xff\x0f\x03\x05\x79\x88\x88\x88\x70"
"\x38\x44\x44\x44\x38\x10\x7c\x10\x30\x28\x24\x24\x28\x20\xe0\xc0"
"\x3c\x24\x3c\x24\x24\xe4\xdc\x18\x10\x54\x38\xee\x38\x54\x10\x00"
"\x10\x10\x10\x7c\x10\x10\x10\x10\x10\x10\x10\xff\x00\x00\x00\x00"
"\x00\x00\x00\xff\x10\x10\x10\x10\x10\x10\x10\xf0\x10\x10\x10\x10"
"\x10\x10\x10\x1f\x10\x10\x10\x10\x10\x10\x10\xff\x10\x10\x10\x10"
"\x10\x10\x10\x10\x10\x10\x10\x10\x00\x00\x00\xff\x00\x00\x00\x00"
"\x00\x00\x00\x1f\x10\x10\x10\x10\x00\x00\x00\xf0\x10\x10\x10\x10"
"\x10\x10\x10\x1f\x00\x00\x00\x00\x10\x10\x10\xf0\x00\x00\x00\x00"
"\x81\x42\x24\x18\x18\x24\x42\x81\x01\x02\x04\x08\x10\x20\x40\x80"
"\x80\x40\x20\x10\x08\x04\x02\x01\x00\x10\x10\xff\x10\x10\x00\x00"
"\x00\x00\x00\x00\x00\x00\x00\x00\x20\x20\x20\x20\x00\x00\x20\x00"
"\x50\x50\x50\x00\x00\x00\x00\x00\x50\x50\xf8\x50\xf8\x50\x50\x00"
"\x20\x78\xa0\x70\x28\xf0\x20\x00\xc0\xc8\x10\x20\x40\x98\x18\x00"
"\x40\xa0\x40\xa8\x90\x98\x60\x00\x10\x20\x40\x00\x00\x00\x00\x00"
"\x10\x20\x40\x40\x40\x20\x10\x00\x40\x20\x10\x10\x10\x20\x40\x00"
"\x20\xa8\x70\x20\x70\xa8\x20\x00\x00\x20\x20\xf8\x20\x20\x00\x00"
"\x00\x00\x00\x00\x00\x20\x20\x40\x00\x00\x00\x78\x00\x00\x00\x00"
"\x00\x00\x00\x00\x00\x60\x60\x00\x00\x00\x08\x10\x20\x40\x80\x00"
"\x70\x88\x98\xa8\xc8\x88\x70\x00\x20\x60\xa0\x20\x20\x20\xf8\x00"
"\x70\x88\x08\x10\x60\x80\xf8\x00\x70\x88\x08\x30\x08\x88\x70\x00"
"\x10\x30\x50\x90\xf8\x10\x10\x00\xf8\x80\xe0\x10\x08\x10\xe0\x00"
"\x30\x40\x80\xf0\x88\x88\x70\x00\xf8\x88\x10\x20\x20\x20\x20\x00"
"\x70\x88\x88\x70\x88\x88\x70\x00\x70\x88\x88\x78\x08\x10\x60\x00"
"\x00\x00\x20\x00\x00\x20\x00\x00\x00\x00\x20\x00\x00\x20\x20\x40"
"\x18\x30\x60\xc0\x60\x30\x18\x00\x00\x00\xf8\x00\xf8\x00\x00\x00"
"\xc0\x60\x30\x18\x30\x60\xc0\x00\x70\x88\x08\x10\x20\x00\x20\x00"
"\x70\x88\x08\x68\xa8\xa8\x70\x00\x20\x50\x88\x88\xf8\x88\x88\x00"
"\xf0\x48\x48\x70\x48\x48\xf0\x00\x30\x48\x80\x80\x80\x48\x30\x00"
"\xe0\x50\x48\x48\x48\x50\xe0\x00\xf8\x80\x80\xf0\x80\x80\xf8\x00"
"\xf8\x80\x80\xf0\x80\x80\x80\x00\x70\x88\x80\xb8\x88\x88\x70\x00"
"\x88\x88\x88\xf8\x88\x88\x88\x00\x70\x20\x20\x20\x20\x20\x70\x00"
"\x38\x10\x10\x10\x90\x90\x60\x00\x88\x90\xa0\xc0\xa0\x90\x88\x00"
"\x80\x80\x80\x80\x80\x80\xf8\x00\x88\xd8\xa8\xa8\x88\x88\x88\x00"
"\x88\xc8\xc8\xa8\x98\x98\x88\x00\x70\x88\x88\x88\x88\x88\x70\x00"
"\xf0\x88\x88\xf0\x80\x80\x80\x00\x70\x88\x88\x88\xa8\x90\x68\x00"
"\xf0\x88\x88\xf0\xa0\x90\x88\x00\x70\x88\x80\x70\x08\x88\x70\x00"
"\xf8\x20\x20\x20\x20\x20\x20\x00\x88\x88\x88\x88\x88\x88\x70\x00"
"\x88\x88\x88\x88\x50\x50\x20\x00\x88\x88\x88\xa8\xa8\xd8\x88\x00"
"\x88\x88\x50\x20\x50\x88\x88\x00\x88\x88\x88\x70\x20\x20\x20\x00"
"\xf8\x08\x10\x20\x40\x80\xf8\x00\x70\x40\x40\x40\x40\x40\x70\x00"
"\x00\x00\x80\x40\x20\x10\x08\x00\x70\x10\x10\x10\x10\x10\x70\x00"
"\x20\x50\x88\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xf8\x00"
"\x40\x20\x10\x00\x00\x00\x00\x00\x00\x00\x70\x08\x78\x88\x78\x00"
"\x80\x80\xb0\xc8\x88\xc8\xb0\x00\x00\x00\x70\x88\x80\x88\x70\x00"
"\x08\x08\x68\x98\x88\x98\x68\x00\x00\x00\x70\x88\xf8\x80\x70\x00"
"\x10\x28\x20\xf8\x20\x20\x20\x00\x00\x00\x68\x98\x98\x68\x08\x70"
"\x80\x80\xf0\x88\x88\x88\x88\x00\x20\x00\x60\x20\x20\x20\x70\x00"
"\x10\x00\x30\x10\x10\x10\x90\x60\x40\x40\x48\x50\x60\x50\x48\x00"
"\x60\x20\x20\x20\x20\x20\x70\x00\x00\x00\xd0\xa8\xa8\xa8\xa8\x00"
"\x00\x00\xb0\xc8\x88\x88\x88\x00\x00\x00\x70\x88\x88\x88\x70\x00"
"\x00\x00\xb0\xc8\xc8\xb0\x80\x80\x00\x00\x68\x98\x98\x68\x08\x08"
"\x00\x00\xb0\xc8\x80\x80\x80\x00\x00\x00\x78\x80\xf0\x08\xf0\x00"
"\x40\x40\xf0\x40\x40\x48\x30\x00\x00\x00\x90\x90\x90\x90\x68\x00"
"\x00\x00\x88\x88\x88\x50\x20\x00\x00\x00\x88\xa8\xa8\xa8\x50\x00"
"\x00\x00\x88\x50\x20\x50\x88\x00\x00\x00\x88\x88\x98\x68\x08\x70"
"\x00\x00\xf8\x10\x20\x40\xf8\x00\x18\x20\x20\x40\x20\x20\x18\x00"
"\x20\x20\x20\x00\x20\x20\x20\x00\xc0\x20\x20\x10\x20\x20\xc0\x00"
"\x40\xa8\x10\x00\x00\x00\x00\x00\x00\x00\x20\x50\xf8\x00\x00\x00"
"\x70\x88\x80\x80\x88\x70\x20\x60\x90\x00\x00\x90\x90\x90\x68\x00"
"\x10\x20\x70\x88\xf8\x80\x70\x00\x20\x50\x70\x08\x78\x88\x78\x00"
"\x48\x00\x70\x08\x78\x88\x78\x00\x20\x10\x70\x08\x78\x88\x78\x00"
"\x20\x00\x70\x08\x78\x88\x78\x00\x00\x70\x80\x80\x80\x70\x10\x60"
"\x20\x50\x70\x88\xf8\x80\x70\x00\x50\x00\x70\x88\xf8\x80\x70\x00"
"\x20\x10\x70\x88\xf8\x80\x70\x00\x50\x00\x00\x60\x20\x20\x70\x00"
"\x20\x50\x00\x60\x20\x20\x70\x00\x40\x20\x00\x60\x20\x20\x70\x00"
"\x50\x00\x20\x50\x88\xf8\x88\x00\x20\x00\x20\x50\x88\xf8\x88\x00"
"\x10\x20\xf8\x80\xf0\x80\xf8\x00\x00\x00\x6c\x12\x7e\x90\x6e\x00"
"\x3e\x50\x90\x9c\xf0\x90\x9e\x00\x60\x90\x00\x60\x90\x90\x60\x00"
"\x90\x00\x00\x60\x90\x90\x60\x00\x40\x20\x00\x60\x90\x90\x60\x00"
"\x40\xa0\x00\xa0\xa0\xa0\x50\x00\x40\x20\x00\xa0\xa0\xa0\x50\x00"
"\x90\x00\x90\x90\xb0\x50\x10\xe0\x50\x00\x70\x88\x88\x88\x70\x00"
"\x50\x00\x88\x88\x88\x88\x70\x00\x20\x20\x78\x80\x80\x78\x20\x20"
"\x18\x24\x20\xf8\x20\xe2\x5c\x00\x88\x50\x20\xf8\x20\xf8\x20\x00"
"\xc0\xa0\xa0\xc8\x9c\x88\x88\x8c\x18\x20\x20\xf8\x20\x20\x20\x40"
"\x10\x20\x70\x08\x78\x88\x78\x00\x10\x20\x00\x60\x20\x20\x70\x00"
"\x20\x40\x00\x60\x90\x90\x60\x00\x20\x40\x00\x90\x90\x90\x68\x00"
"\x50\xa0\x00\xa0\xd0\x90\x90\x00\x28\x50\x00\xc8\xa8\x98\x88\x00"
"\x00\x70\x08\x78\x88\x78\x00\xf8\x00\x60\x90\x90\x90\x60\x00\xf0"
"\x20\x00\x20\x40\x80\x88\x70\x00\x00\x00\x00\xf8\x80\x80\x00\x00"
"\x00\x00\x00\xf8\x08\x08\x00\x00\x84\x88\x90\xa8\x54\x84\x08\x1c"
"\x84\x88\x90\xa8\x58\xa8\x3c\x08\x20\x00\x00\x20\x20\x20\x20\x00"
"\x00\x00\x24\x48\x90\x48\x24\x00\x00\x00\x90\x48\x24\x48\x90\x00"
"\x28\x50\x20\x50\x88\xf8\x88\x00\x28\x50\x70\x08\x78\x88\x78\x00"
"\x28\x50\x00\x70\x20\x20\x70\x00\x28\x50\x00\x20\x20\x20\x70\x00"
"\x28\x50\x00\x70\x88\x88\x70\x00\x50\xa0\x00\x60\x90\x90\x60\x00"
"\x28\x50\x00\x88\x88\x88\x70\x00\x50\xa0\x00\xa0\xa0\xa0\x50\x00"
"\xfc\x48\x48\x48\xe8\x08\x50\x20\x00\x50\x00\x50\x50\x50\x10\x20"
"\xc0\x44\xc8\x54\xec\x54\x9e\x04\x10\xa8\x40\x00\x00\x00\x00\x00"
"\x00\x20\x50\x88\x50\x20\x00\x00\x88\x10\x20\x40\x80\x28\x00\x00"
"\x7c\xa8\xa8\x68\x28\x28\x28\x00\x38\x40\x30\x48\x48\x30\x08\x70"
"\x00\x00\x00\x00\x00\x00\xff\xff\xf0\xf0\xf0\xf0\x0f\x0f\x0f\x0f"
"\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00"
"\x00\x00\x00\x3c\x3c\x00\x00\x00\xff\xff\xff\xff\xff\xff\x00\x00"
"\xc0\xc0\xc0\xc0\xc0\xc0\xc0\xc0\x0f\x0f\x0f\x0f\xf0\xf0\xf0\xf0"
"\xfc\xfc\xfc\xfc\xfc\xfc\xfc\xfc\x03\x03\x03\x03\x03\x03\x03\x03"
"\x3f\x3f\x3f\x3f\x3f\x3f\x3f\x3f\x11\x22\x44\x88\x11\x22\x44\x88"
"\x88\x44\x22\x11\x88\x44\x22\x11\xfe\x7c\x38\x10\x00\x00\x00\x00"
"\x00\x00\x00\x00\x10\x38\x7c\xfe\x80\xc0\xe0\xf0\xe0\xc0\x80\x00"
"\x01\x03\x07\x0f\x07\x03\x01\x00\xff\x7e\x3c\x18\x18\x3c\x7e\xff"
"\x81\xc3\xe7\xff\xff\xe7\xc3\x81\xf0\xf0\xf0\xf0\x00\x00\x00\x00"
"\x00\x00\x00\x00\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x00\x00\x00\x00"
"\x00\x00\x00\x00\xf0\xf0\xf0\xf0\x33\x33\xcc\xcc\x33\x33\xcc\xcc"
"\x00\x20\x20\x50\x50\x88\xf8\x00\x20\x20\x70\x20\x70\x20\x20\x00"
"\x00\x00\x00\x50\x88\xa8\x50\x00\xff\xff\xff\xff\xff\xff\xff\xff"
"\x00\x00\x00\x00\xff\xff\xff\xff\xf0\xf0\xf0\xf0\xf0\xf0\xf0\xf0"
"\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x0f\xff\xff\xff\xff\x00\x00\x00\x00"
"\x00\x00\x68\x90\x90\x90\x68\x00\x30\x48\x48\x70\x48\x48\x70\xc0"
"\xf8\x88\x80\x80\x80\x80\x80\x00\xf8\x50\x50\x50\x50\x50\x98\x00"
"\xf8\x88\x40\x20\x40\x88\xf8\x00\x00\x00\x78\x90\x90\x90\x60\x00"
"\x00\x50\x50\x50\x50\x68\x80\x80\x00\x50\xa0\x20\x20\x20\x20\x00"
"\xf8\x20\x70\xa8\xa8\x70\x20\xf8\x20\x50\x88\xf8\x88\x50\x20\x00"
"\x70\x88\x88\x88\x50\x50\xd8\x00\x30\x40\x40\x20\x50\x50\x50\x20"
"\x00\x00\x00\x50\xa8\xa8\x50\x00\x08\x70\xa8\xa8\xa8\x70\x80\x00"
"\x38\x40\x80\xf8\x80\x40\x38\x00\x70\x88\x88\x88\x88\x88\x88\x00"
"\x00\xf8\x00\xf8\x00\xf8\x00\x00\x20\x20\xf8\x20\x20\x00\xf8\x00"
"\xc0\x30\x08\x30\xc0\x00\xf8\x00\x18\x60\x80\x60\x18\x00\xf8\x00"
"\x10\x28\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\xa0\x40"
"\x00\x20\x00\xf8\x00\x20\x00\x00\x00\x50\xa0\x00\x50\xa0\x00\x00"
"\x00\x18\x24\x24\x18\x00\x00\x00\x00\x30\x78\x78\x30\x00\x00\x00"
"\x00\x00\x00\x00\x30\x00\x00\x00\x3e\x20\x20\x20\xa0\x60\x20\x00"
"\xa0\x50\x50\x50\x00\x00\x00\x00\x40\xa0\x20\x40\xe0\x00\x00\x00"
"\x00\x38\x38\x38\x38\x38\x38\x00\x00\x00\x00\x00\x00\x00\x00";

/* ---- overlay state ---- */
#define SCR_W      480
#define SCR_H      272
#define GLYPH      8              /* font cell size                            */
#define ADVANCE    6              /* per-char step (glyphs are 5px -> 1px gap)  */
#define COLON_KERN  2             /* pull ':' left to hug the prev char         */
#define OSD_TICKS  32             /* ~2.5 s (worker loops, ~80 ms each)         */
#define OSD_MSG_TICKS 63          /* one-off messages: ~5 s                      */

static char osd_text[80];               /* holds the 2-line DEBUG string */
static volatile int osd_ticks = 0;
static volatile int osd_lock  = 0;      /* >0: a locked message owns the OSD */
static void *orig_setframebuf = NULL;   /* the real SetFrameBuf */

/* ---- poll-draw (fallback) state ----
 * For games that don't drive the hook: read the live framebuffer (GetFrameBuf) and
 * draw into it from a thread, synced to vblank. */
static int (*p_getframebuf)(void **topaddr, int *bufwidth, int *pixfmt, int sync) = NULL;
static int (*p_waitvblankstart)(void) = NULL;

static int osd_draw_mode = 0;           /* 0=auto 1=hook-only 2=poll-only */

/* bumped by hook AND poll (drives the visibility timer) vs hook-only (auto test). */
static volatile unsigned int osd_last_hook_us = 0;
static volatile int osd_painting = 0;   /* hook/poll mutual exclusion (visual only) */
static volatile int osd_draw_run = 0;   /* poll-draw thread run flag */
static SceUID osd_draw_thid = -1;

/* ---- flip-chain framebuffer set ----
 * Double/triple-buffered games cycle 2-3 buffers; painting only the live one
 * flickers. Cache recently-seen distinct buffers and paint them all each pass.
 * Cache is VRAM-only (always mapped; stale write = 1-frame glitch, not a fault),
 * dropped on geometry change, and expired fast. RAM framebuffers are covered by the
 * live in-hook paint instead, never cached. */
#define FB_MAX 4
#define FB_EXPIRE_US 200000u            /* drop an address not re-seen for 200 ms */
#define FB_SETTLE_US 1200000u           /* pause poll-draw ~1.2s after a mode change
                                           (game<->XMB) - the buffers are torn down then */
static volatile u32 fb_addr[FB_MAX];
static volatile unsigned int fb_seen[FB_MAX];
static volatile int fb_bw = 0;
static volatile int fb_pf = -1;
static volatile unsigned int fb_transition_us = 0;   /* last geometry change (mode switch) */

/* strip the 0x40000000 uncached bit so cached/uncached dedupe to one entry */
static u32 fb_norm(u32 a) { return a & 0x0FFFFFFFu; }

/* VRAM only (2 MB eDRAM); see flip-chain note above. */
static int fb_plausible(u32 a)
{
	a = fb_norm(a);
	return (a >= 0x04000000u && a < 0x04200000u);       /* VRAM only */
}

/* Remember a framebuffer the system just used (hook topaddr or poll GetFrameBuf). */
static void fb_register(u32 top, int bw, int pf, unsigned int now)
{
	int i, slot;
	if(bw <= 0 || pf < 0 || pf > 3 || !fb_plausible(top)) return;
	top = fb_norm(top);

	if(bw != fb_bw || pf != fb_pf)          /* geometry changed -> drop the set */
	{
		for(i = 0; i < FB_MAX; i++) { fb_addr[i] = 0; fb_seen[i] = 0; }
		fb_bw = bw; fb_pf = pf;
		fb_transition_us = now;             /* mode switch -> pause poll-draw to settle */
	}
	for(i = 0; i < FB_MAX; i++)              /* known -> refresh timestamp */
		if(fb_addr[i] == top) { fb_seen[i] = now; return; }

	slot = 0;                                /* else empty/oldest slot */
	for(i = 0; i < FB_MAX; i++)
	{
		if(fb_addr[i] == 0) { slot = i; break; }
		if(fb_seen[i] < fb_seen[slot]) slot = i;
	}
	fb_addr[slot] = top; fb_seen[slot] = now;
}

/* diagnostic counters (read by osd_log_status, written by the hook/notify) */
static volatile int osd_hook_calls   = 0;   /* times our SetFrameBuf hook ran     */
volatile unsigned int osd_last_frame_us = 0; /* sceKernelGetSystemTimeLow at last frame */
static volatile int osd_draw_calls   = 0;   /* times it actually drew (ticks > 0) */
static volatile int osd_notify_calls = 0;   /* times a brightness change notified */
static volatile u32 osd_last_top     = 0;   /* last framebuffer addr the hook saw */
static volatile int osd_last_bw      = 0;   /* last bufferwidth                   */
static volatile int osd_last_pf      = -1;  /* last pixelformat                   */
static volatile int osd_last_sync    = -1;  /* last sync mode (0=immediate,1=next)*/


/* "Brightness Level" per language (PSP_SYSTEMPARAM_LANGUAGE_*) for Latin scripts
 * (ASCII, accents dropped). Non-Latin use word images; entries here are an unused
 * English fallback. */
static const char *const bright_word[] = {
	"Brightness Level",      /*  0 Japanese (image) */
	"Brightness Level",      /*  1 English   */
	"Niveau de luminosite",  /*  2 French    */
	"Nivel de brillo",       /*  3 Spanish   */
	"Helligkeitsstufe",      /*  4 German    */
	"Livello luminosita",    /*  5 Italian   */
	"Helderheidsniveau",     /*  6 Dutch     */
	"Nivel de brilho",       /*  7 Portuguese*/
	"Brightness Level",      /*  8 Russian (image) */
	"Brightness Level",      /*  9 Korean  (image) */
	"Brightness Level",      /* 10 Chinese T (image) */
	"Brightness Level",      /* 11 Chinese S (image) */
};
#define OSD_NLANGS ((int)(sizeof(bright_word) / sizeof(bright_word[0])))

static int osd_lang = 1;   /* system-language index (default English)            */

void osd_set_language(int lang)
{
	osd_lang = (lang >= 0 && lang < OSD_NLANGS) ? lang : 1;   /* else English */
}

/* ---- non-Latin "Brightness Level" word images ----
 * Pre-rendered 1bpp bitmaps (11px tall, MSB-first, row-major) for the scripts the
 * font can't draw; blitted and scaled like the font, with the number after. */
#define OSD_WORD_H 11
typedef struct { int w, rowbytes; const unsigned char *bits; } OsdWord;

/* jp: 明るさレベル  66x11 */
static const unsigned char word_jp_bits[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7b, 0xc3, 0xe0, 0x10, 0x10, 0x00, 0x00, 0x24, 0x00, 0x48, 0x40, 0x40, 0x1e, 0x10, 0x01, 0x84, 0x24, 0x00, 0x4b, 0xc0, 0x80, 0xf8, 0x10, 0x01, 0x40, 0x24, 0x00, 0x78, 0x41, 0x00, 0x08, 0x10, 0x02, 0x20, 0x24, 0x00, 0x4c, 0x43, 0xe0, 0x04, 0x10, 0x04, 0x30, 0x24, 0x00, 0x4f, 0xc6, 0x10, 0x7e, 0x10, 0x44, 0x18, 0x04, 0x80, 0x7c, 0x41, 0x08, 0x80, 0x10, 0x80, 0x0c, 0x44, 0x80, 0x44, 0x42, 0xd0, 0x80, 0x13, 0x00, 0x04, 0x45, 0x00, 0x08, 0x42, 0x70, 0x7c, 0x1c, 0x00, 0x00, 0x86, 0x00, 0x08, 0xc1, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
/* ru: Уровень яркости  92x11 */
static const unsigned char word_ru_bits[] = { 0xc2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0xf0, 0xe7, 0x87, 0x22, 0x40, 0x1f, 0x78, 0x93, 0x87, 0x7d, 0x30, 0x24, 0xd9, 0x34, 0xc9, 0xa2, 0x40, 0x11, 0x6c, 0xa4, 0xc9, 0x91, 0x30, 0x3c, 0x8b, 0x14, 0xc8, 0xa2, 0x40, 0x11, 0x44, 0xac, 0x48, 0x11, 0x70, 0x18, 0x8b, 0x17, 0x9f, 0xbe, 0x78, 0x0f, 0x44, 0xcc, 0x58, 0x11, 0x50, 0x18, 0x8b, 0x14, 0xc8, 0x22, 0x4c, 0x19, 0x44, 0xac, 0x48, 0x11, 0x90, 0x10, 0x99, 0x34, 0x48, 0xa2, 0x4c, 0x11, 0x4c, 0xb4, 0xc9, 0x91, 0x90, 0x60, 0xf0, 0xe7, 0x87, 0x22, 0x78, 0x31, 0x78, 0x93, 0x87, 0x11, 0x10, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00 };
/* kr: 밝기 레벨  44x11 */
static const unsigned char word_kr_bits[] = { 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x44, 0x9f, 0x20, 0x72, 0x92, 0xa0, 0x7c, 0x81, 0x20, 0x1a, 0x9e, 0xa0, 0x44, 0xc1, 0x20, 0x0a, 0x92, 0xa0, 0x44, 0x82, 0x20, 0x7e, 0x9e, 0xa0, 0x78, 0x82, 0x20, 0x42, 0x80, 0xa0, 0x00, 0x04, 0x20, 0x42, 0x87, 0xe0, 0x09, 0x98, 0x20, 0x7a, 0x80, 0x20, 0x38, 0x90, 0x20, 0x02, 0x8f, 0xe0, 0x40, 0x80, 0x20, 0x02, 0x88, 0x00, 0x38, 0x00, 0x20, 0x00, 0x8f, 0xf0 };
/* zh: 亮度等级  44x11 */
static const unsigned char word_zh_bits[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x80, 0xc4, 0x10, 0x00, 0x7f, 0xc7, 0xf8, 0xff, 0x17, 0xc0, 0x3f, 0x85, 0x21, 0x42, 0x2e, 0x40, 0x20, 0x87, 0xf8, 0x62, 0x3a, 0xe0, 0x3f, 0x85, 0x60, 0xfe, 0x13, 0x20, 0x7f, 0xc4, 0x01, 0xff, 0x33, 0x40, 0x42, 0x47, 0xf0, 0x04, 0x32, 0xc0, 0x12, 0x09, 0x21, 0xff, 0x06, 0xc0, 0x12, 0x48, 0xe0, 0x64, 0x3d, 0xe0, 0x63, 0xcb, 0x38, 0x0c, 0x0b, 0x20 };

/* Per-language word image; bits==NULL means "use the ASCII bright_word[] instead". */
static const OsdWord osd_word[OSD_NLANGS] = {
	{ 66, 9, word_jp_bits },   /*  0 Japanese  */
	{  0, 0, 0           },   /*  1 English   */
	{  0, 0, 0           },   /*  2 French    */
	{  0, 0, 0           },   /*  3 Spanish   */
	{  0, 0, 0           },   /*  4 German    */
	{  0, 0, 0           },   /*  5 Italian   */
	{  0, 0, 0           },   /*  6 Dutch     */
	{  0, 0, 0           },   /*  7 Portuguese*/
	{ 92,12, word_ru_bits },   /*  8 Russian   */
	{ 44, 6, word_kr_bits },   /*  9 Korean    */
	{ 44, 6, word_zh_bits },   /* 10 Chinese T */
	{ 44, 6, word_zh_bits },   /* 11 Chinese S */
};

/* Set = blit this word image before osd_text (which is then just ":NN"). NULL =
 * pure-ASCII osd_text. Reset by every other text setter. */
static const OsdWord *osd_word_cur = 0;

/* Append the level digits to p. */
static char *osd_put_num(char *p, int level)
{
	if(level < 0)   level = 0;
	if(level > 100) level = 100;
	if(level >= 100)     { *p++ = '1'; *p++ = '0'; *p++ = '0'; }
	else if(level >= 10) { *p++ = (char)('0' + level / 10); *p++ = (char)('0' + level % 10); }
	else                 { *p++ = (char)('0' + level); }
	return p;
}

/* Show "<Brightness>:<level>". Latin word is ASCII; others use a word image and
 * osd_text holds only ":NN". Pure memory - safe in the display hook. */
void osd_notify(int level)
{
	const OsdWord *wd = &osd_word[osd_lang];
	char *p = osd_text;

	if(osd_lock > 0) return;

	if(wd->bits)                         /* non-Latin: image + ":NN" */
	{
		osd_word_cur = wd;
	}
	else                                 /* Latin: "<word>: NN" */
	{
		const char *w = bright_word[osd_lang];
		osd_word_cur = 0;
		while(*w) *p++ = *w++;
	}
	*p++ = ':';
	if(!wd->bits) *p++ = ' ';            /* Latin: 1 space after colon */
	p = osd_put_num(p, level);
	*p = 0;

	osd_ticks = OSD_TICKS;
	osd_notify_calls++;
}

/* Debug: show raw patch args, "L=<dec> U=0x<hex>". */
void osd_probe(int level, unsigned int unk1)
{
	static const char hexd[] = "0123456789ABCDEF";
	char *p = osd_text;
	unsigned int u;
	int sh;

	if(osd_lock > 0) return;
	osd_word_cur = 0;                    /* ASCII-only line */

	*p++ = 'L'; *p++ = '=';
	if(level < 0) { *p++ = '-'; u = (unsigned int)(-level); }
	else            u = (unsigned int)level;
	{
		char tmp[12]; int n = 0;
		if(u == 0) tmp[n++] = '0';
		while(u) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
		while(n) *p++ = tmp[--n];
	}

	*p++ = ' '; *p++ = 'U'; *p++ = '='; *p++ = '0'; *p++ = 'x';
	for(sh = 28; sh >= 0; sh -= 4) *p++ = hexd[(unk1 >> sh) & 0xF];
	*p = 0;

	osd_ticks = OSD_TICKS;
	osd_notify_calls++;
}

/* Debug: show "<tag> L=<level>" so the patch's decision is visible on-screen. */
void osd_note(const char *tag, int level)
{
	char *p = osd_text;
	unsigned int u;
	int i = 0;

	if(osd_lock > 0) return;
	osd_word_cur = 0;                    /* ASCII-only line */

	while(tag[i] && i < 16) { *p++ = tag[i]; i++; }
	*p++ = ' '; *p++ = 'L'; *p++ = '=';

	if(level < 0) { *p++ = '-'; u = (unsigned int)(-level); }
	else            u = (unsigned int)level;
	{
		char tmp[12]; int n = 0;
		if(u == 0) tmp[n++] = '0';
		while(u) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
		while(n) *p++ = tmp[--n];
	}
	*p = 0;

	osd_ticks = OSD_TICKS;
	osd_notify_calls++;
}

#define OSD_HOOK_LIVE_US 200000u   /* hook counts as "live" if it fired within 200 ms */

/* Which path is on screen now: "api-hook" (in the SetFrameBuf hook) or "fb-poll"
 * (poll thread into the live buffer). In auto, picks per whether the hook is live. */
const char *osd_draw_path_name(void)
{
	if(osd_draw_mode == 1) return "api-hook";
	if(osd_draw_mode == 2) return "fb-poll";
	/* auto: report whichever is actually carrying the overlay right now */
	if((sceKernelGetSystemTimeLow() - osd_last_hook_us) <= OSD_HOOK_LIVE_US)
		return "api-hook";
	return "fb-poll";
}

int osd_is_visible(void){ return osd_ticks > 0; }

/* signed decimal appender */
static char *osd_put_dec(char *p, int v)
{
	unsigned int u;
	char tmp[12]; int n = 0;
	if(v < 0) { *p++ = '-'; u = (unsigned int)(-v); } else u = (unsigned int)v;
	if(u == 0) tmp[n++] = '0';
	while(u) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
	while(n) *p++ = tmp[--n];
	return p;
}
static char *osd_put_str(char *p, const char *s){ while(*s) *p++ = *s++; return p; }

/* Two-line DEBUG overlay (split on '\n' so it fits at larger sizes):
 *   "BB <ver> DEBUG: L=<fw> U=<level>" / "event=<tag> draw=<path>". */
void osd_debug(const char *event, int level, unsigned int unk1)
{
	char *p = osd_text;
	int i = 0;

	if(osd_lock > 0) return;
	osd_word_cur = 0;                    /* ASCII-only (2-line) */

	p = osd_put_str(p, "BB " BB_VERSION " DEBUG: L=");
	p = osd_put_dec(p, level);
	p = osd_put_str(p, " U=");
	p = osd_put_dec(p, (int)unk1);
	*p++ = '\n';                                  /* line 2 */
	p = osd_put_str(p, "event=");
	while(event[i] && i < 12) { *p++ = event[i]; i++; }
	p = osd_put_str(p, " draw=");
	p = osd_put_str(p, osd_draw_path_name());
	*p = 0;

	osd_ticks = OSD_TICKS;
	osd_notify_calls++;
}

/* One-off message (e.g. first-run credit); locks the OSD so nothing overwrites it. */
void osd_message(const char *s)
{
	char *p = osd_text;
	int i = 0;
	osd_word_cur = 0;                    /* ASCII-only message */
	while(s[i] && i < 47) { *p++ = s[i]; i++; }
	*p = 0;

	osd_ticks = OSD_MSG_TICKS;
	osd_lock  = OSD_MSG_TICKS;
	osd_notify_calls++;
}

void osd_tick(void)
{
	/* Only count down while frames are actually drawing (so a message shown before
	 * the XMB renders keeps its full duration). */
	static unsigned int prev_frame = 0;
	unsigned int f = osd_last_frame_us;
	if(f == prev_frame) return;     /* no new frame -> hold */
	prev_frame = f;
	if(osd_ticks > 0) osd_ticks--;
	if(osd_lock  > 0) osd_lock--;
}

/* Dump the counters to the log (worker-thread context only). */
void osd_log_status(void)
{
	if(!log_is_on()) return;
	log_kv("osd.hook_calls",   osd_hook_calls);
	log_kv("osd.draw_calls",   osd_draw_calls);
	log_kv("osd.notify_calls", osd_notify_calls);
	log_kx("osd.last_topaddr", osd_last_top);
	log_kv("osd.last_bufwidth", osd_last_bw);
	log_kv("osd.last_pixfmt",  osd_last_pf);
	log_kv("osd.last_sync",    osd_last_sync);
}

/* ---- OSD style (set from the ini before install) ---- */
static int osd_scale  = 1;   /* integer multiplier 1x..4x (pixel-crisp)         */
static int osd_pos    = 1;   /* 1 = bottom, 2 = top                            */
static int osd_fg_idx = 2;   /* text colour index (white)                      */
static int osd_bg_idx = 1;   /* plate colour index (black)                     */
static int osd_bg_none = 0;  /* 1 = transparent (no plate, osd_bg_colour=0)    */

/* PSP-themed palette, RGB888 (the .ini colour numbering). col32/col16 convert it. */
static const unsigned char osd_pal[][3] = {
	{   0,   0,   0 },   /*  1 piano black    */
	{ 255, 255, 255 },   /*  2 ceramic white  */
	{ 214, 184, 122 },   /*  3 champagne gold */
	{ 200, 205, 212 },   /*  4 ice silver     */
	{ 150, 230, 180 },   /*  5 mint green     */
	{ 205, 228, 245 },   /*  6 felicia blue   */
	{  90, 165, 255 },   /*  7 vivid blue     */
	{ 255,   0,   0 },   /*  8 radiant red    */
	{   0,   0, 255 },   /*  9 blue           */
	{ 180, 150, 225 },   /* 10 lilac purple   */
	{ 255, 190, 220 },   /* 11 blossom pink   */
	{ 235,  90, 150 },   /* 12 pink           */
	{ 255, 217,  15 },   /* 13 yellow         */
	{ 130, 205,  95 },   /* 14 spirited green */
	{  48, 195, 185 },   /* 15 turquoise      */
	{ 158, 115,  72 },   /* 16 matte bronze   */
	{ 150,  25,  30 },   /* 17 deep red       */
	{ 165, 170, 218 },   /* 18 lavender       */
};
#define OSD_NCOLOURS ((int)(sizeof(osd_pal) / sizeof(osd_pal[0])))

void osd_set_style(int text_colour, int bg_colour, int size, int position)
{
	if(text_colour >= 1 && text_colour <= OSD_NCOLOURS) osd_fg_idx = text_colour;
	if(bg_colour   >= 1 && bg_colour   <= OSD_NCOLOURS) osd_bg_idx = bg_colour;
	osd_bg_none = (bg_colour == 0);   /* 0 = transparent (no plate) */
	osd_scale = (size >= 1 && size <= 4) ? size : 1;   /* 1x..4x, else 1x */
	osd_pos   = (position == 2) ? 2 : 1;
}

/* colour index -> 32bpp ABGR8888 (exact). */
static u32 col32(int idx)
{
	const unsigned char *c;
	if(idx < 1 || idx > OSD_NCOLOURS) idx = 2;
	c = osd_pal[idx - 1];
	return 0xFF000000u | ((u32)c[2] << 16) | ((u32)c[1] << 8) | (u32)c[0];
}

/* colour index -> 16bpp for the pixelformat. Black/white exact; tints best-effort. */
static u16 col16(int idx, int pf)
{
	const unsigned char *c;
	unsigned int r, g, b;
	if(idx < 1 || idx > OSD_NCOLOURS) idx = 2;
	c = osd_pal[idx - 1];
	r = c[0]; g = c[1]; b = c[2];
	switch(pf) {
	case 0: /* 5650: R[0:4] G[5:10] B[11:15] */
		return (u16)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
	case 1: /* 5551: R[0:4] G[5:9] B[10:14] A[15] */
		return (u16)(0x8000u | (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
	case 2: /* 4444: R[0:3] G[4:7] B[8:11] A[12:15] */
		return (u16)(0xF000u | (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8));
	}
	return (idx == 1) ? 0x0000 : 0xFFFF;
}

/* osd_text may contain '\n' (the DEBUG view uses 2 lines). */
#define OSD_MAXLINES 3

/* ---- plate fill ---- */
static void fill_plate16(u16 *fb, int stride, int x, int y, int w, int h, u16 bg)
{
	int bx, by;
	for(by = y; by < y + h; by++)
	{
		if(by < 0 || by >= SCR_H) continue;
		for(bx = x; bx < x + w; bx++)
		{
			if(bx < 0 || bx >= SCR_W || bx >= stride) continue;
			fb[by * stride + bx] = bg;
		}
	}
}
static void fill_plate32(u32 *fb, int stride, int x, int y, int w, int h, u32 bg)
{
	int bx, by;
	for(by = y; by < y + h; by++)
	{
		if(by < 0 || by >= SCR_H) continue;
		for(bx = x; bx < x + w; bx++)
		{
			if(bx < 0 || bx >= SCR_W || bx >= stride) continue;
			fb[by * stride + bx] = bg;
		}
	}
}

/* ---- line layout ----
 * A non-leading ':' is kerned left (and its advance shortened) so it hugs the
 * previous letter; a leading ':' (after a word image) is left alone. */
static int char_advance(const char *text, int i, int sc)
{
	if(text[i] == ':' && i > 0)                    return (ADVANCE - COLON_KERN) * sc;
	if(text[i] == ' ' && i > 0 && text[i-1] == ':') return (ADVANCE - 2) * sc;  /* ": N" tighter */
	return ADVANCE * sc;
}
static int char_kern(const char *text, int i, int sc)
{
	return (text[i] == ':' && i > 0) ? COLON_KERN * sc : 0;
}
/* Pixel width of a line. */
static int line_w(const char *text, int len, int sc)
{
	int i, w = 0;
	for(i = 0; i < len; i++) w += char_advance(text, i, sc);
	return w;
}

/* ---- one line of glyphs (no plate), each pixel an sc x sc block ---- */
static void draw_line16(u16 *fb, int stride, int x0, int y0, const char *text, int len, u16 fg, int sc)
{
	int ci, r, c, sx, sy, x = x0;
	for(ci = 0; ci < len; ci++)
	{
		const u8 *g = &bb_font[((u8)text[ci]) * 8];
		int gx = x - char_kern(text, ci, sc);
		for(r = 0; r < 8; r++)
		{
			u8 row = g[r];
			for(c = 0; c < 8; c++)
			{
				if(!(row & (128 >> c))) continue;
				for(sy = 0; sy < sc; sy++)
				{
					int py = y0 + r * sc + sy;
					if(py < 0 || py >= SCR_H) continue;
					for(sx = 0; sx < sc; sx++)
					{
						int px = gx + c * sc + sx;
						if(px < 0 || px >= SCR_W || px >= stride) continue;
						fb[py * stride + px] = fg;
					}
				}
			}
		}
		x += char_advance(text, ci, sc);
	}
}
static void draw_line32(u32 *fb, int stride, int x0, int y0, const char *text, int len, u32 fg, int sc)
{
	int ci, r, c, sx, sy, x = x0;
	for(ci = 0; ci < len; ci++)
	{
		const u8 *g = &bb_font[((u8)text[ci]) * 8];
		int gx = x - char_kern(text, ci, sc);
		for(r = 0; r < 8; r++)
		{
			u8 row = g[r];
			for(c = 0; c < 8; c++)
			{
				if(!(row & (128 >> c))) continue;
				for(sy = 0; sy < sc; sy++)
				{
					int py = y0 + r * sc + sy;
					if(py < 0 || py >= SCR_H) continue;
					for(sx = 0; sx < sc; sx++)
					{
						int px = gx + c * sc + sx;
						if(px < 0 || px >= SCR_W || px >= stride) continue;
						fb[py * stride + px] = fg;
					}
				}
			}
		}
		x += char_advance(text, ci, sc);
	}
}

/* ---- one word image (no plate), scaled sc x ---- */
static void blit_word16(u16 *fb, int stride, int x0, int y0, const OsdWord *wd, u16 fg, int sc)
{
	int r, c, sx, sy;
	for(r = 0; r < OSD_WORD_H; r++)
	{
		const unsigned char *rowp = wd->bits + r * wd->rowbytes;
		for(c = 0; c < wd->w; c++)
		{
			if(!(rowp[c >> 3] & (0x80 >> (c & 7)))) continue;
			for(sy = 0; sy < sc; sy++)
			{
				int py = y0 + r * sc + sy;
				if(py < 0 || py >= SCR_H) continue;
				for(sx = 0; sx < sc; sx++)
				{
					int px = x0 + c * sc + sx;
					if(px < 0 || px >= SCR_W || px >= stride) continue;
					fb[py * stride + px] = fg;
				}
			}
		}
	}
}
static void blit_word32(u32 *fb, int stride, int x0, int y0, const OsdWord *wd, u32 fg, int sc)
{
	int r, c, sx, sy;
	for(r = 0; r < OSD_WORD_H; r++)
	{
		const unsigned char *rowp = wd->bits + r * wd->rowbytes;
		for(c = 0; c < wd->w; c++)
		{
			if(!(rowp[c >> 3] & (0x80 >> (c & 7)))) continue;
			for(sy = 0; sy < sc; sy++)
			{
				int py = y0 + r * sc + sy;
				if(py < 0 || py >= SCR_H) continue;
				for(sx = 0; sx < sc; sx++)
				{
					int px = x0 + c * sc + sx;
					if(px < 0 || px >= SCR_W || px >= stride) continue;
					fb[py * stride + px] = fg;
				}
			}
		}
	}
}

/* Draw the overlay: one plate, lines centred (split on '\n'). When osd_word_cur is
 * set, blit the word image before the ":NN" text (baseline-aligned). */
static void osd_draw(void *topaddr, int bufferwidth, int pixelformat)
{
	int sc, gh, gap, nlines, i;
	int lstart[OSD_MAXLINES], llen[OSD_MAXLINES];
	int maxw, total_h, plate_x, plate_y, y, k, s;
	void *fb;

	if(!topaddr || bufferwidth <= 0 || osd_text[0] == 0) return;

	sc  = osd_scale;
	gh  = GLYPH * sc;
	gap = 2 * sc;                          /* blank rows between lines */

	fb = (void *)(((u32)topaddr) | 0x40000000);   /* uncached mirror (no cache flush) */

	/* ---- non-Latin: [word image][":NN"] ---- */
	if(osd_word_cur)
	{
		const OsdWord *wd = osd_word_cur;
		int imgW = wd->w * sc, imgH = OSD_WORD_H * sc;
		int tlen = 0; while(osd_text[tlen]) tlen++;
		int textW = line_w(osd_text, tlen, sc);
		int lineW = imgW + textW;
		int px = (SCR_W - lineW) / 2; if(px < 0) px = 0;
		int py = (osd_pos == 2) ? 8 : (SCR_H - imgH - 6);
		int ty = py + (imgH - gh);          /* baseline-align number to word */

		if(pixelformat == 3)
		{
			u32 fg = col32(osd_fg_idx), bg = col32(osd_bg_idx);
			if(!osd_bg_none) fill_plate32((u32 *)fb, bufferwidth, px - 3, py - 2, lineW + 6, imgH + 4, bg);
			blit_word32((u32 *)fb, bufferwidth, px, py, wd, fg, sc);
			draw_line32((u32 *)fb, bufferwidth, px + imgW, ty, osd_text, tlen, fg, sc);
		}
		else if(pixelformat >= 0 && pixelformat <= 2)
		{
			u16 fg = col16(osd_fg_idx, pixelformat), bg = col16(osd_bg_idx, pixelformat);
			if(!osd_bg_none) fill_plate16((u16 *)fb, bufferwidth, px - 3, py - 2, lineW + 6, imgH + 4, bg);
			blit_word16((u16 *)fb, bufferwidth, px, py, wd, fg, sc);
			draw_line16((u16 *)fb, bufferwidth, px + imgW, ty, osd_text, tlen, fg, sc);
		}
		return;
	}

	/* split into lines on '\n' */
	nlines = 0; s = 0;
	for(k = 0; ; k++)
	{
		char ch = osd_text[k];
		if(ch == '\n' || ch == 0)
		{
			if(nlines < OSD_MAXLINES) { lstart[nlines] = s; llen[nlines] = k - s; nlines++; }
			if(ch == 0) break;
			s = k + 1;
		}
	}
	if(nlines == 0) return;

	maxw = 0;
	for(i = 0; i < nlines; i++)
	{
		int lw = line_w(&osd_text[lstart[i]], llen[i], sc);
		if(lw > maxw) maxw = lw;
	}
	total_h = nlines * gh + (nlines - 1) * gap;
	plate_x = (SCR_W - maxw) / 2; if(plate_x < 0) plate_x = 0;
	plate_y = (osd_pos == 2) ? 8 : (SCR_H - total_h - 6);   /* top : bottom */

	if(pixelformat == 3)                       /* 8888 */
	{
		u32 fg = col32(osd_fg_idx), bg = col32(osd_bg_idx);
		if(!osd_bg_none) fill_plate32((u32 *)fb, bufferwidth, plate_x - 3, plate_y - 2, maxw + 6, total_h + 4, bg);
		y = plate_y;
		for(i = 0; i < nlines; i++)
		{
			int lx = (SCR_W - line_w(&osd_text[lstart[i]], llen[i], sc)) / 2; if(lx < 0) lx = 0;
			draw_line32((u32 *)fb, bufferwidth, lx, y, &osd_text[lstart[i]], llen[i], fg, sc);
			y += gh + gap;
		}
	}
	else if(pixelformat >= 0 && pixelformat <= 2)  /* 565 / 5551 / 4444 */
	{
		u16 fg = col16(osd_fg_idx, pixelformat), bg = col16(osd_bg_idx, pixelformat);
		if(!osd_bg_none) fill_plate16((u16 *)fb, bufferwidth, plate_x - 3, plate_y - 2, maxw + 6, total_h + 4, bg);
		y = plate_y;
		for(i = 0; i < nlines; i++)
		{
			int lx = (SCR_W - line_w(&osd_text[lstart[i]], llen[i], sc)) / 2; if(lx < 0) lx = 0;
			draw_line16((u16 *)fb, bufferwidth, lx, y, &osd_text[lstart[i]], llen[i], fg, sc);
			y += gh + gap;
		}
	}
	/* any other format: leave the frame untouched */
}

/* Our SetFrameBuf hook: draw, then chain to the original. No file I/O here. */
static int osd_set_frame_buf(void *topaddr, int bufferwidth, int pixelformat, int sync)
{
	unsigned int now = sceKernelGetSystemTimeLow();
	osd_hook_calls++;
	osd_last_hook_us  = now;       /* hook-only heartbeat (auto-mode test) */
	osd_last_frame_us = now;       /* drives osd_tick */
	osd_last_top = (u32)topaddr;
	osd_last_bw  = bufferwidth;
	osd_last_pf  = pixelformat;
	osd_last_sync = sync;

	fb_register((u32)topaddr, bufferwidth, pixelformat, now);   /* remember flip-chain buffer */

	/* Draw in-hook unless poll-only mode; guard against the poll thread. */
	if(osd_ticks > 0 && osd_draw_mode != 2 && !osd_painting)
	{
		osd_painting = 1;
		osd_draw_calls++;
		osd_draw(topaddr, bufferwidth, pixelformat);
		osd_painting = 0;
	}

	return ((int (*)(void *, int, int, int))orig_setframebuf)
	       (topaddr, bufferwidth, pixelformat, sync);
}

/* One paint pass: the live buffer (always mapped) + the cached VRAM flip-chain. */
static void osd_paint_pass(void *top, int bw, int pf, unsigned int now)
{
	int i;
	osd_painting = 1;
	if(top && bw > 0) osd_draw(top, bw, pf);          /* live buffer (RAM or VRAM) */
	if(fb_bw > 0 && fb_pf >= 0)                        /* cached extra buffers */
		for(i = 0; i < FB_MAX; i++)
		{
			u32 a = fb_addr[i];
			if(a && a != fb_norm((u32)top) && (now - fb_seen[i]) <= FB_EXPIRE_US)
				osd_draw((void *)a, fb_bw, fb_pf);
		}
	osd_painting = 0;
}

/* Read whichever buffer is currently on screen. k1 cleared for the driver calls. */
static void *osd_live_buf(int *bw, int *pf)
{
	void *top = NULL;
	u32 k1 = pspSdkSetK1(0);
	if(p_getframebuf(&top, bw, pf, 0 /*PSP_DISPLAY_SETBUF_IMMEDIATE*/) < 0) top = NULL;
	pspSdkSetK1(k1);
	return top;
}

#define HALF_FRAME_US 8000   /* mid-frame gap for the 2nd paint (~half a 60fps frame) */

/* Poll-draw: paint at vblank, then again mid-frame, to beat double-buffer flicker.
 * Draw-thread only. */
static void osd_draw_poll(void)
{
	void *top;
	int bw = 0, pf = 0;
	unsigned int now;
	u32 k1;

	if(!p_getframebuf || osd_ticks <= 0 || osd_text[0] == 0) return;
	if(osd_painting) return;                 /* hook is mid-paint - skip this pass */

	k1 = pspSdkSetK1(0);
	if(p_waitvblankstart) p_waitvblankstart();   /* align paint #1 to vblank */
	pspSdkSetK1(k1);

	/* Bail before any paint if a shutdown began during the vblank wait - quitting
	 * tears the buffers down and a stray write would reboot. (Re-checked before #2.) */
	if(!osd_draw_run || osd_ticks <= 0) return;

	top = osd_live_buf(&bw, &pf);
	now = sceKernelGetSystemTimeLow();
	if(top && bw > 0) fb_register((u32)top, bw, pf, now);
	osd_paint_pass(top, bw, pf, now);             /* paint #1 */
	osd_last_frame_us = now;
	osd_draw_calls++;

	/* paint #2 mid-frame (catch a flip/re-render since the vblank paint) */
	sceKernelDelayThread(HALF_FRAME_US);
	if(!osd_draw_run || osd_ticks <= 0) return;
	top = osd_live_buf(&bw, &pf);
	now = sceKernelGetSystemTimeLow();
	if(top && bw > 0) fb_register((u32)top, bw, pf, now);
	osd_paint_pass(top, bw, pf, now);
}

/* Draw thread: nap until the OSD is visible, then poll-draw (unless hook-only).
 * Self-paces via the vblank wait inside osd_draw_poll. */
static int OsdDrawThread(SceSize args, void *argp)
{
	(void)args; (void)argp;

	while(osd_draw_run)
	{
		if(p_getframebuf && osd_draw_mode != 1 && osd_ticks > 0 && osd_text[0])
		{
			unsigned int now = sceKernelGetSystemTimeLow();
			if((now - fb_transition_us) < FB_SETTLE_US)
				sceKernelDelayThread(30000);  /* mode switch (game<->XMB) - let it settle */
			else
				osd_draw_poll();
		}
		else
			sceKernelDelayThread(30000);      /* nap */
	}

	return 0;   /* don't self-delete; osd_shutdown joins + deletes us */
}

void osd_set_draw_mode(int mode)
{
	osd_draw_mode = (mode >= 0 && mode <= 2) ? mode : 0;
}

void osd_shutdown(void)
{
	osd_draw_run = 0;        /* stop the thread (and any in-flight poll) now */

	if(osd_draw_thid >= 0)
	{
		/* Join before module_stop unloads us (else the thread could wake into freed
		 * code). Timeout is just a safety net. */
		SceUInt timeout = 1000000;
		sceKernelWaitThreadEnd(osd_draw_thid, &timeout);
		sceKernelDeleteThread(osd_draw_thid);
		osd_draw_thid = -1;
	}
}

int osd_install(void)
{
	u32 addr;
	u32 *slot;
	u32 ints;

	log_msg("osd_install: enter");

	/* sceDisplaySetFrameBuf NID 0x289D82FE (user lib, then kernel lib). */
	addr = find_export("sceDisplay_Service", "sceDisplay", 0x289D82FE);
	log_kx("osd_install.addr_sceDisplay", addr);
	if(!addr)
	{
		addr = find_export("sceDisplay_Service", "sceDisplay_driver", 0x289D82FE);
		log_kx("osd_install.addr_sceDisplay_driver", addr);
	}
	if(!addr) { log_msg("osd_install: FAIL (function not found)"); return 0; }

	/* Prefer patching the syscall-table slot (catches user-mode callers). */
	slot = find_syscall_slot(addr);
	log_kx("osd_install.syscall_slot", (u32)slot);

	if(slot)
	{
		orig_setframebuf = (void *)addr;       /* call the original directly */
		ints = pspSdkDisableInterrupts();
		*slot = (u32)osd_set_frame_buf;
		pspSdkEnableInterrupts(ints);
		log_msg("osd_install: OK (syscall slot patched)");
	}
	else
	{
		orig_setframebuf = hijack_install(addr, osd_set_frame_buf);   /* fallback */
		log_msg("osd_install: OK (function entry hijacked - fallback)");
	}

	sceKernelDcacheWritebackAll();
	sceKernelIcacheClearAll();

	/* Resolve poll-draw helpers (GetFrameBuf 0xEEDA2E54, WaitVblankStart 0x984C27E7);
	 * if missing, poll just never runs. */
	if(osd_draw_mode != 1)
	{
		u32 gfb = find_export("sceDisplay_Service", "sceDisplay", 0xEEDA2E54);
		u32 wvb = find_export("sceDisplay_Service", "sceDisplay", 0x984C27E7);
		if(!gfb) gfb = find_export("sceDisplay_Service", "sceDisplay_driver", 0xEEDA2E54);
		if(!wvb) wvb = find_export("sceDisplay_Service", "sceDisplay_driver", 0x984C27E7);
		p_getframebuf     = (int (*)(void **, int *, int *, int))gfb;
		p_waitvblankstart = (int (*)(void))wvb;
		log_kx("osd_install.addr_getframebuf",   gfb);
		log_kx("osd_install.addr_waitvblank",    wvb);

		if(p_getframebuf)
		{
			osd_draw_run  = 1;
			osd_draw_thid = sceKernelCreateThread("BetterBright_osd", OsdDrawThread,
			                                      0x20, 0x2000, 0, NULL);
			if(osd_draw_thid >= 0)
			{
				sceKernelStartThread(osd_draw_thid, 0, NULL);
				log_msg("osd_install: poll-draw thread started");
			}
			else
			{
				osd_draw_run = 0;
				log_msg("osd_install: poll-draw thread FAILED to create");
			}
		}
		else
			log_msg("osd_install: GetFrameBuf not found - poll-draw disabled");
	}

	return 1;
}
