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
#include "version.h"

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

static char osd_text[80];   /* must hold the DEBUG line: "BB <ver> DEBUG: L=.. U=.. event=.. draw=.." */
static volatile int osd_ticks = 0;
static volatile int osd_lock  = 0;      /* >0: a locked message owns the OSD, no overwrite */
static void *orig_setframebuf = NULL;   /* the real SetFrameBuf (called directly) */

/* ---- Mode 2 (poll-draw) state ---------------------------------------------
 * Some games never drive the sceDisplaySetFrameBuf slot we hook, so the in-hook
 * draw (Mode 1) never runs for them and the OSD is absent. The fallback, used by
 * the stable PSP-HUD plugin, is to ask the display service what is CURRENTLY on
 * screen (sceDisplayGetFrameBuf - returns the live buffer regardless of who set
 * it) and draw straight into it from a thread, synced to vblank. We reuse the
 * exact same draw16/draw32 routines, the real pixelformat/stride from the driver,
 * and the real buffer address - i.e. none of the hardcoded-VRAM / fixed-8888 /
 * no-vblank mistakes that crashed the old "Brightness-Control" reference plugin. */
static int (*p_getframebuf)(void **topaddr, int *bufwidth, int *pixfmt, int sync) = NULL;
static int (*p_waitvblankstart)(void) = NULL;

/* 0 = auto (hook, poll only when the hook is idle), 1 = hook only, 2 = poll only */
static int osd_draw_mode = 0;

/* osd_last_frame_us (below) is "something drew a frame" and is bumped by BOTH the
 * hook and the poll path, so the visibility timer counts down in either mode.
 * osd_last_hook_us is bumped ONLY by the hook, so auto-mode can tell whether the
 * in-hook draw is actually reaching the screen for the current game. */
static volatile unsigned int osd_last_hook_us = 0;

/* Cheap mutual exclusion so the hook and the poll thread never paint at once.
 * A stray race is only ever visual (both paths are bounds-checked), never a
 * crash - mirrors PSP-HUD's single 'wait' flag. */
static volatile int osd_painting = 0;

/* Poll-draw thread lifecycle. */
static volatile int osd_draw_run = 0;
static SceUID osd_draw_thid = -1;

/* ---- flip-chain framebuffer set -------------------------------------------
 * Games double/triple-buffer: they cycle the displayed frame between 2-3 buffers.
 * If we paint only the one buffer GetFrameBuf hands back, the frames showing the
 * OTHER buffer have no text -> fast flicker. So we remember the recently-seen
 * distinct framebuffer addresses (fed by BOTH the hook and the poll) and stamp the
 * text into ALL of them each pass; whichever buffer is shown next already has it.
 *
 * STABILITY: writing to a stale framebuffer pointer is the one real risk here, so
 * the cache is guarded four ways - entries expire fast (a freed buffer stops being
 * re-seen and ages out), the whole set is dropped the moment bufferwidth/format
 * changes (that's when a game reallocates buffers), only VRAM addresses are ever
 * cached (VRAM is fixed hardware memory, always mapped - a stale write there is a
 * one-frame glitch, never a fault), and the thread is fully joined before unload.
 *
 * The buffer GetFrameBuf returns "live" each pass is a separate matter: it's the
 * one the display is actively scanning, so it's ALWAYS mapped (even RAM ones, even
 * mid-teardown) and is painted directly - that's how RAM-framebuffer games like
 * Lego Batman get covered without putting a RAM pointer in the cache. */
#define FB_MAX 4
#define FB_EXPIRE_US 200000u            /* drop an address not re-seen for 200 ms */
static volatile u32 fb_addr[FB_MAX];
static volatile unsigned int fb_seen[FB_MAX];
static volatile int fb_bw = 0;
static volatile int fb_pf = -1;

/* Normalise to the cached base address (strip the 0x40000000 uncached bit) so the
 * same buffer seen cached/uncached dedupes to one entry. */
static u32 fb_norm(u32 a) { return a & 0x0FFFFFFFu; }

/* Accept only VRAM addresses (the 2 MB eDRAM at 0x04000000..0x04200000) into the
 * async flip-chain set. VRAM is fixed hardware memory - always mapped - so even a
 * stale write is a harmless one-frame glitch, never a fault. Main-RAM framebuffers
 * are deliberately excluded: they're the ones a game frees on exit, and chasing a
 * freed RAM pointer from the poll thread is what reboots the PSP. A live RAM buffer
 * is still covered by the synchronous in-hook draw, which only ever runs on the
 * exact frame the game itself is presenting (so it can't be stale). */
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

	if(bw != fb_bw || pf != fb_pf)          /* geometry changed -> drop the old set */
	{
		for(i = 0; i < FB_MAX; i++) { fb_addr[i] = 0; fb_seen[i] = 0; }
		fb_bw = bw; fb_pf = pf;
	}
	for(i = 0; i < FB_MAX; i++)              /* already known -> refresh timestamp   */
		if(fb_addr[i] == top) { fb_seen[i] = now; return; }

	slot = 0;                                /* else take an empty or the oldest slot */
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

/* In auto mode, the hook is "live" (driving this game) if it fired this recently;
 * otherwise the poll thread is what's reaching the screen. */
#define OSD_HOOK_LIVE_US 200000u   /* 200 ms */

const char *osd_draw_path_name(void)
{
	if(osd_draw_mode == 1) return "hook";
	if(osd_draw_mode == 2) return "poll";
	/* auto: report whichever is actually carrying the overlay right now */
	if((sceKernelGetSystemTimeLow() - osd_last_hook_us) <= OSD_HOOK_LIVE_US)
		return "auto-hook";
	return "auto-poll";
}

int osd_is_visible(void){ return osd_ticks > 0; }

/* tiny unsigned/signed decimal appender for the debug line */
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

/* DEBUG overlay line:
 *   "BB <ver> DEBUG: L=<fw> U=<plugin> event=<tag> draw=<path>"
 * L = firmware/native backlight step (44/60/72/84), U = our actual brightness,
 * draw = which path is reaching the screen (hook / poll / auto-hook / auto-poll).
 * Fires on every trigger (press / dim / wake / idle). */
void osd_debug(const char *event, int level, unsigned int unk1)
{
	char *p = osd_text;
	int i = 0;

	if(osd_lock > 0) return;

	p = osd_put_str(p, "BB " BB_VERSION " DEBUG: L=");
	p = osd_put_dec(p, level);
	p = osd_put_str(p, " U=");
	p = osd_put_dec(p, (int)unk1);
	p = osd_put_str(p, " event=");
	while(event[i] && i < 12) { *p++ = event[i]; i++; }
	p = osd_put_str(p, " draw=");
	p = osd_put_str(p, osd_draw_path_name());
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
	unsigned int now = sceKernelGetSystemTimeLow();
	osd_hook_calls++;
	osd_last_hook_us  = now;       /* hook-only heartbeat (auto-mode fallback test) */
	osd_last_frame_us = now;       /* "screen is drawing" heartbeat (drives osd_tick) */
	osd_last_top = (u32)topaddr;
	osd_last_bw  = bufferwidth;
	osd_last_pf  = pixelformat;
	osd_last_sync = sync;

	/* This buffer is part of the game's flip chain - remember it so the poll path
	 * (and the line below) can keep every buffer stamped, not just the live one. */
	fb_register((u32)topaddr, bufferwidth, pixelformat, now);

	/* Mode 1 (hook) or Mode 0 (auto) draw in-hook here. Mode 2 (poll only) leaves
	 * the frame to the poll thread. The paint guard keeps us off a frame the poll
	 * thread is mid-write on (it draws into the live buffer, which may be this one). */
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

/* One paint pass: the live buffer (any region - it's the one the display is
 * scanning, so always mapped) plus every still-fresh CACHED buffer (VRAM only).
 * Painting the live buffer directly is what covers RAM-framebuffer games. */
static void osd_paint_pass(void *top, int bw, int pf, unsigned int now)
{
	int i;
	osd_painting = 1;
	if(top && bw > 0) osd_draw(top, bw, pf);          /* live buffer (RAM or VRAM) */
	if(fb_bw > 0 && fb_pf >= 0)                        /* extra flip-chain buffers  */
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

/* Mid-frame gap for the second paint: roughly half a 60 fps frame, so we catch a
 * game that flips or re-renders the front buffer partway through the frame. */
#define HALF_FRAME_US 8000

/* ---- Mode 2: poll-draw into the live framebuffer(s) ------------------------
 * Twice per frame (at vblank, then mid-frame) stamp the OSD into the live buffer
 * and the cached VRAM flip-chain. The vblank-synced first paint plus the mid-frame
 * second paint together beat the every-other-frame flicker on double/triple-
 * buffered games. Draw-thread context only (k1 handling is in osd_live_buf). */
static void osd_draw_poll(void)
{
	void *top;
	int bw = 0, pf = 0;
	unsigned int now;
	u32 k1;

	if(!p_getframebuf || osd_ticks <= 0 || osd_text[0] == 0) return;
	if(osd_painting) return;                 /* hook is mid-paint - skip this pass */

	k1 = pspSdkSetK1(0);
	if(p_waitvblankstart) p_waitvblankstart();   /* align paint #1 to blanking */
	pspSdkSetK1(k1);

	/* Bail before touching ANY buffer if a shutdown began while we were parked in
	 * the vblank wait - quitting a game tears the framebuffers down underneath us,
	 * and one stray paint into freed memory is a reboot. Load-bearing for the
	 * "OSD up when you quit" crash; re-checked again before paint #2. */
	if(!osd_draw_run || osd_ticks <= 0) return;

	top = osd_live_buf(&bw, &pf);
	now = sceKernelGetSystemTimeLow();
	if(top && bw > 0) fb_register((u32)top, bw, pf, now);
	osd_paint_pass(top, bw, pf, now);             /* paint #1 (at vblank) */
	osd_last_frame_us = now;
	osd_draw_calls++;

	/* paint #2, partway through the frame, to re-stamp a buffer the game flipped to
	 * or re-rendered since the vblank paint. */
	sceKernelDelayThread(HALF_FRAME_US);
	if(!osd_draw_run || osd_ticks <= 0) return;
	top = osd_live_buf(&bw, &pf);
	now = sceKernelGetSystemTimeLow();
	if(top && bw > 0) fb_register((u32)top, bw, pf, now);
	osd_paint_pass(top, bw, pf, now);
}

/* Background draw thread. Idle (30 ms naps) until the OSD is visible, then (unless
 * forced to hook-only) stamps the flip-chain buffers every vblank. In auto mode
 * this runs alongside the in-hook draw - painting the whole buffer set is what
 * stops the every-other-frame flicker on double/triple-buffered games, and it's
 * harmless on games the hook already covers (those buffers just get the same text
 * twice). WaitVblankStart inside osd_draw_poll paces it to ~once per frame, so it
 * never spins the CPU. */
static int OsdDrawThread(SceSize args, void *argp)
{
	(void)args; (void)argp;

	while(osd_draw_run)
	{
		if(p_getframebuf && osd_draw_mode != 1 && osd_ticks > 0 && osd_text[0])
			osd_draw_poll();                  /* self-paces via vblank wait */
		else
			sceKernelDelayThread(30000);      /* nothing to do - nap 30 ms */
	}

	/* Just return - DON'T self-delete. osd_shutdown() joins us with WaitThreadEnd
	 * and then deletes the thread, so the PRX is never unloaded while we're alive. */
	return 0;
}

void osd_set_draw_mode(int mode)
{
	osd_draw_mode = (mode >= 0 && mode <= 2) ? mode : 0;
}

void osd_shutdown(void)
{
	osd_draw_run = 0;        /* tells the thread (and any in-flight poll) to stop NOW */

	if(osd_draw_thid >= 0)
	{
		/* Deterministically wait for the thread to actually leave its entry point
		 * before we let module_stop unload us. A blind delay isn't enough: if the
		 * thread is parked in WaitVblankStart when we unload, it wakes into freed
		 * code = reboot. WaitThreadEnd returns as soon as it exits (a vblank is
		 * ~16 ms away); the 1 s timeout is just a safety net. */
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

	/* Resolve the poll-draw helpers. These are stock display exports; if either is
	 * missing we simply never poll (auto mode = exactly today's behaviour, Mode 2 =
	 * no-op). sceDisplayGetFrameBuf NID 0xEEDA2E54, WaitVblankStart NID 0x984C27E7. */
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
