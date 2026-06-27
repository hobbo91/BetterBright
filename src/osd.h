#ifndef OSD_H__
#define OSD_H__

/*
 * On-screen brightness display ("Display Brightness: NN").
 *
 * Stable technique: we hook the kernel sceDisplaySetFrameBuf and draw the text
 * straight into the buffer that is about to be shown, every frame. That is why
 * it appears in XMB, games and PS1 alike without flicker - unlike drawing from a
 * background thread, which races whatever is rendering and flickers.
 *
 * When osd_enable is 0 the hook is never installed, so the plugin behaves exactly
 * as if this feature did not exist (zero overhead, zero risk).
 */

/* Install the sceDisplaySetFrameBuf hook. Call once at startup, only if enabled.
 * Returns 1 on success, 0 if the function could not be found/patched.
 *
 * Also resolves sceDisplayGetFrameBuf / sceDisplayWaitVblankStart and starts the
 * poll-draw thread (Mode 2). The thread only ever draws when the OSD is visible
 * AND (per draw mode) the in-hook draw isn't reaching the screen - so games that
 * already work via the hook are never touched by it. */
int  osd_install(void);

/* Stop the poll-draw thread. Call from module_stop before freeing anything. */
void osd_shutdown(void);

/* Select how the overlay reaches the screen. Call once before osd_install().
 *   0 = auto  : draw in the sceDisplaySetFrameBuf hook; if that hook isn't firing
 *               (some games never drive it), fall back to drawing into the live
 *               framebuffer from the poll thread.
 *   1 = hook  : in-hook draw only (the original behaviour, zero poll activity).
 *   2 = poll  : never draw in-hook; always draw into the live framebuffer. */
void osd_set_draw_mode(int mode);

/* Show "Display Brightness: <level>" for a short time. Called on each user
 * brightness change. Safe to call from the display hook (no syscalls). */
void osd_notify(int level);
void osd_probe(int level, unsigned int unk1);  /* debug: show raw firmware args */
void osd_note(const char *tag, int level);     /* debug: show "<tag> L=<level>" */
void osd_debug(const char *event, int level, unsigned int unk1); /* "DEBUG L=.. U=.. event=.." */
void osd_message(const char *s);               /* one-off message (first-run credit) */

/* Which path the OSD is currently reaching the screen through, as a short static
 * string: "hook", "poll", "auto-hook" or "auto-poll". Used by the DEBUG overlay
 * line and by the log. */
const char *osd_draw_path_name(void);

/* 1 while the OSD timer is counting (overlay on screen), else 0. */
int osd_is_visible(void);

/* Set OSD colours/size/position from the ini. Call once before osd_install().
 * text_colour/bg_colour: 1=black 2=white 3=red 4=green 5=blue.
 * size: 1=normal 2=large.  position: 1=bottom 2=top. */
void osd_set_style(int text_colour, int bg_colour, int size, int position);

extern volatile unsigned int osd_last_frame_us; /* time of last drawn frame (µs)   */

/* Tick down the visibility timer. Call once per worker loop (~80 ms). */
void osd_tick(void);

/* Dump hook/draw/notify counters + last framebuffer params to the log. Call from
 * the worker thread only (does file I/O when logging is enabled). */
void osd_log_status(void);

#endif
