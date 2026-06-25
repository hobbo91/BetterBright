/*
 * osd.c  -  on-screen "Display Brightness: NN" overlay for BetterBright.
 *
 * HOW IT STAYS STABLE
 * -------------------
 * We hijack the kernel sceDisplaySetFrameBuf (sceDisplay_Service / sceDisplay_
 * driver, NID 0x289D82FE). That function is the single point every context -
 * XMB, games, PS1 - funnels through to put a frame on screen. Our hook draws the
 * text into the very buffer being shown and then calls the original. So the
 * overlay is always on the displayed frame, in any context, with no flicker.
 *
 * This is deliberately NOT the approach of the old buggy brightness OSD, which
 * drew from a background thread into whatever GetFrameBuf returned - racing the
 * game's own rendering, hence the flicker/instability.
 *
 * Everything here is pure memory writes (an 8x8 bitmap font straight into VRAM)
 * with no syscalls, so it is safe to run inside the display hook.
 */

#include <pspkernel.h>
#include <module2.h>         /* SceModule2 - the CORRECT firmware module layout  */
#include <psploadcore.h>     /* SceLibraryEntryTable (entry format is fine)      */
#include <pspsdk.h>          /* pspSdkDisableInterrupts / EnableInterrupts       */
#include <string.h>
#include "osd.h"
#include "log.h"

/* NOTE: we must read the module's export table through SceModule2, not the stock
 * pspsdk SceModule. The CFW firmware struct has two extra words (mpid_text /
 * mpid_data) before ent_top, so pspsdk's SceModule puts ent_top 8 bytes too
 * early and the walk reads garbage - that was the v1 bug (export lookup = 0).
 * SceModule2 is the same struct main.c already uses for text_addr. */

/* ---- syscall-table hook (the mechanism the stable PSP-HUD uses) -----------
 * The XMB and every game call sceDisplaySetFrameBuf from USER mode, which enters
 * the kernel through the syscall table. Redirecting the table slot is what
 * catches those callers. cop0 $12 holds a pointer to the running syscall table;
 * this is the same approach PSP-HUD uses. */
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

/* Find an export address by module / library / NID, walking the CORRECT
 * SceModule2 export table. */
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

/* ---- instruction-hijack fallback ------------------------------------------
 * If the function isn't reachable as a syscall slot, redirect its entry instead.
 * Saves the first two instructions into a trampoline so the original is still
 * callable. (Used only if the syscall-slot patch can't be applied.) */
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


/* 8x8, 1bpp bitmap font (MSB = leftmost column), one glyph = 8 bytes, indexed by
 * char code. This is the public-domain pspsdk debug font, embedded (renamed) so
 * the plugin has no dependency on -lpspdebug's data. */
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

/* ---- overlay state ---------------------------------------------------------*/
#define SCR_W      480
#define SCR_H      272
#define GLYPH      8              /* font cell size                            */
#define ADVANCE    7              /* horizontal step per character             */
#define OSD_TICKS  32             /* worker loops (~80 ms each) ~= 2.5 s        */
#define OSD_MSG_TICKS 63          /* one-off messages: ~5 s                      */

static char osd_text[48];
static volatile int osd_ticks = 0;
static volatile int osd_lock  = 0;      /* >0: a locked message owns the OSD, no overwrite */
static void *orig_setframebuf = NULL;   /* the real SetFrameBuf (called directly) */

/* diagnostic counters (read by osd_log_status, written by the hook/notify) */
static volatile int osd_hook_calls   = 0;   /* times our SetFrameBuf hook ran     */
volatile unsigned int osd_last_frame_us = 0; /* sceKernelGetSystemTimeLow at last frame */
static volatile int osd_draw_calls   = 0;   /* times it actually drew (ticks > 0) */
static volatile int osd_notify_calls = 0;   /* times a brightness change notified */
static volatile u32 osd_last_top     = 0;   /* last framebuffer addr the hook saw */
static volatile int osd_last_bw      = 0;   /* last bufferwidth                   */
static volatile int osd_last_pf      = -1;  /* last pixelformat                   */
static volatile int osd_last_sync    = -1;  /* last sync mode (0=immediate,1=next)*/

static int osd_strlen(const char *s){ int n = 0; while(s[n]) n++; return n; }

/* Build "Display Brightness: <level>" and show it for OSD_TICKS worker loops.
 * Pure string work - safe to call from the display hook. */
void osd_notify(int level)
{
	static const char lbl[] = "Display Brightness: ";
	char *p = osd_text;
	int i = 0;

	if(osd_lock > 0) return;             /* a locked message owns the OSD */

	while(lbl[i]) { *p++ = lbl[i]; i++; }

	if(level < 0)   level = 0;
	if(level > 100) level = 100;
	if(level >= 100)     { *p++ = '1'; *p++ = '0'; *p++ = '0'; }
	else if(level >= 10) { *p++ = (char)('0' + level / 10); *p++ = (char)('0' + level % 10); }
	else                 { *p++ = (char)('0' + level); }
	*p = 0;

	osd_ticks = OSD_TICKS;
	osd_notify_calls++;
}

/* Probe (debug build): show the raw (level, unk1) the firmware passes into the
 * brightness patch, so a genuine Display press and a wake-from-dim can be
 * compared on-screen. level as signed decimal, unk1 as hex (it's unknown, so
 * show every bit). Output e.g. "L=80 U=0x00000000". */
void osd_probe(int level, unsigned int unk1)
{
	static const char hexd[] = "0123456789ABCDEF";
	char *p = osd_text;
	unsigned int u;
	int sh;

	if(osd_lock > 0) return;

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

/* Debug build: "DEBUG L=<native> B=<brightness> event=<tag>" - L is the firmware's
 * raw backlight step (44/60/72/84), B is our actual brightness (the ini value we
 * set). Shows every trigger (press / dim / wake / idle / OFF / ON). */
void osd_debug(const char *event, int level, unsigned int unk1)
{
	static const char pre[] = "DEBUG L=";
	char *p = osd_text;
	unsigned int u;
	int i = 0;

	if(osd_lock > 0) return;

	while(pre[i]) { *p++ = pre[i]; i++; }

	if(level < 0) { *p++ = '-'; u = (unsigned int)(-level); }
	else            u = (unsigned int)level;
	{ char tmp[12]; int n = 0;
	  if(u == 0) tmp[n++] = '0';
	  while(u) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
	  while(n) *p++ = tmp[--n]; }

	*p++ = ' '; *p++ = 'B'; *p++ = '=';
	u = unk1;
	{ char tmp[12]; int n = 0;
	  if(u == 0) tmp[n++] = '0';
	  while(u) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
	  while(n) *p++ = tmp[--n]; }

	*p++ = ' '; *p++ = 'e'; *p++ = 'v'; *p++ = 'e'; *p++ = 'n'; *p++ = 't'; *p++ = '=';
	i = 0;
	while(event[i] && i < 12) { *p++ = event[i]; i++; }
	*p = 0;

	osd_ticks = OSD_TICKS;
	osd_notify_calls++;
}

/* Show an arbitrary one-off message (e.g. the first-run credit) for ~3 s and
 * LOCK the OSD for that time, so a brightness change or debug line can't paint
 * over it. */
void osd_message(const char *s)
{
	char *p = osd_text;
	int i = 0;
	while(s[i] && i < 47) { *p++ = s[i]; i++; }
	*p = 0;

	osd_ticks = OSD_MSG_TICKS;
	osd_lock  = OSD_MSG_TICKS;
	osd_notify_calls++;
}

void osd_tick(void)
{
	/* Only count down while the screen is actually drawing frames. A message put
	 * up before the XMB starts rendering (e.g. the first-run credit) then keeps
	 * its full visible duration instead of expiring during the blank early boot. */
	static unsigned int prev_frame = 0;
	unsigned int f = osd_last_frame_us;
	if(f == prev_frame) return;     /* no new frame since last tick -> hold */
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

/* ---- OSD style (set once from the ini before the hook is installed) -------- */
static int osd_scale  = 1;   /* 1 = normal, 2 = large                          */
static int osd_pos    = 1;   /* 1 = bottom, 2 = top                            */
static int osd_fg_idx = 2;   /* text colour index (white)                      */
static int osd_bg_idx = 1;   /* plate colour index (black)                     */

/* PSP-console-themed palette, RGB888. col32/col16 convert to the live pixel
 * format, so adding a colour here is the only change needed. Order is the .ini
 * numbering; 1/2/8/9 reproduce the old black/white/red/blue exactly. */
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
	osd_scale = (size == 2) ? 2 : 1;
	osd_pos   = (position == 2) ? 2 : 1;
}

/* colour index -> 32bpp ABGR8888 (the XMB format). Exact. */
static u32 col32(int idx)
{
	const unsigned char *c;
	if(idx < 1 || idx > OSD_NCOLOURS) idx = 2;
	c = osd_pal[idx - 1];
	return 0xFF000000u | ((u32)c[2] << 16) | ((u32)c[1] << 8) | (u32)c[0];
}

/* colour index -> 16bpp for the given game pixelformat. Converted from the same
 * RGB table; black/white stay exact, the tints assume the PSP's usual ABGR bit
 * order (R in the low bits) and are best-effort - they may read slightly off in
 * some games' framebuffers, which is why colour is documented as a stretch. */
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

/* 16bpp draw (565 / 5551 / 4444): scaled text on a coloured plate. */
static void draw16(u16 *fb, int stride, int x0, int y0, u16 fg, u16 bg, int sc)
{
	int len = osd_strlen(osd_text);
	int adv = ADVANCE * sc;
	int w   = len * adv;
	int gh  = GLYPH * sc;
	int bx, by, ci, r, c, sx, sy;

	for(by = y0 - 2; by < y0 + gh + 2; by++)
	{
		if(by < 0 || by >= SCR_H) continue;
		for(bx = x0 - 3; bx < x0 + w + 3; bx++)
		{
			if(bx < 0 || bx >= SCR_W || bx >= stride) continue;
			fb[by * stride + bx] = bg;
		}
	}
	for(ci = 0; osd_text[ci]; ci++)
	{
		const u8 *g = &bb_font[((u8)osd_text[ci]) * 8];
		int gx = x0 + ci * adv;
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
	}
}

/* 32bpp draw (8888): scaled text on a coloured plate. */
static void draw32(u32 *fb, int stride, int x0, int y0, u32 fg, u32 bg, int sc)
{
	int len = osd_strlen(osd_text);
	int adv = ADVANCE * sc;
	int w   = len * adv;
	int gh  = GLYPH * sc;
	int bx, by, ci, r, c, sx, sy;

	for(by = y0 - 2; by < y0 + gh + 2; by++)
	{
		if(by < 0 || by >= SCR_H) continue;
		for(bx = x0 - 3; bx < x0 + w + 3; bx++)
		{
			if(bx < 0 || bx >= SCR_W || bx >= stride) continue;
			fb[by * stride + bx] = bg;
		}
	}
	for(ci = 0; osd_text[ci]; ci++)
	{
		const u8 *g = &bb_font[((u8)osd_text[ci]) * 8];
		int gx = x0 + ci * adv;
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
	}
}

/* Draw the overlay into the buffer that is about to be displayed. */
static void osd_draw(void *topaddr, int bufferwidth, int pixelformat)
{
	int len, w, x0, y0, sc, gh;
	void *fb;

	if(!topaddr || bufferwidth <= 0 || osd_text[0] == 0) return;

	sc  = osd_scale;
	len = osd_strlen(osd_text);
	w   = len * ADVANCE * sc;
	gh  = GLYPH * sc;
	x0  = (SCR_W - w) / 2; if(x0 < 0) x0 = 0;
	y0  = (osd_pos == 2) ? 8 : (SCR_H - gh - 6);   /* top : bottom, off the edge */

	/* uncached mirror so the display sees the writes without a cache flush */
	fb = (void *)(((u32)topaddr) | 0x40000000);

	if(pixelformat == 3)                       /* 8888 */
		draw32((u32 *)fb, bufferwidth, x0, y0,
		       col32(osd_fg_idx), col32(osd_bg_idx), sc);
	else if(pixelformat >= 0 && pixelformat <= 2)  /* 565 / 5551 / 4444 */
		draw16((u16 *)fb, bufferwidth, x0, y0,
		       col16(osd_fg_idx, pixelformat), col16(osd_bg_idx, pixelformat), sc);
	/* any other format: leave the frame untouched */
}

/* Our replacement for sceDisplaySetFrameBuf: draw, then chain to the original.
 * No file I/O here - only cheap counter updates the worker later logs. */
static int osd_set_frame_buf(void *topaddr, int bufferwidth, int pixelformat, int sync)
{
	osd_hook_calls++;
	osd_last_frame_us = sceKernelGetSystemTimeLow();   /* "screen is drawing" heartbeat */
	osd_last_top = (u32)topaddr;
	osd_last_bw  = bufferwidth;
	osd_last_pf  = pixelformat;
	osd_last_sync = sync;

	if(osd_ticks > 0)
	{
		osd_draw_calls++;
		osd_draw(topaddr, bufferwidth, pixelformat);
	}

	return ((int (*)(void *, int, int, int))orig_setframebuf)
	       (topaddr, bufferwidth, pixelformat, sync);
}

int osd_install(void)
{
	u32 addr;
	u32 *slot;
	u32 ints;

	log_msg("osd_install: enter");

	/* The export the XMB and games actually call lives in the USER "sceDisplay"
	 * library (PSP-HUD hooks exactly this). Fall back to the kernel library. */
	addr = find_export("sceDisplay_Service", "sceDisplay", 0x289D82FE);
	log_kx("osd_install.addr_sceDisplay", addr);
	if(!addr)
	{
		addr = find_export("sceDisplay_Service", "sceDisplay_driver", 0x289D82FE);
		log_kx("osd_install.addr_sceDisplay_driver", addr);
	}
	if(!addr) { log_msg("osd_install: FAIL (function not found)"); return 0; }

	/* Primary: patch the syscall-table slot (catches user-mode callers). */
	slot = find_syscall_slot(addr);
	log_kx("osd_install.syscall_slot", (u32)slot);

	if(slot)
	{
		orig_setframebuf = (void *)addr;       /* original intact -> call directly */
		ints = pspSdkDisableInterrupts();
		*slot = (u32)osd_set_frame_buf;
		pspSdkEnableInterrupts(ints);
		log_msg("osd_install: OK (syscall slot patched)");
	}
	else
	{
		/* Fallback: redirect the function entry (still catches syscall callers,
		 * which jump to the entry, plus any kernel callers). */
		orig_setframebuf = hijack_install(addr, osd_set_frame_buf);
		log_msg("osd_install: OK (function entry hijacked - fallback)");
	}

	sceKernelDcacheWritebackAll();
	sceKernelIcacheClearAll();
	return 1;
}
