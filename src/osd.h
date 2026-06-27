#ifndef OSD_H__
#define OSD_H__

/*
 * On-screen "Brightness: NN" overlay. Drawn in the sceDisplaySetFrameBuf hook, with
 * a poll-draw thread fallback for games that don't drive it (see osd_set_draw_mode).
 * When osd_enable=0 nothing is installed.
 */

/* Install the hook + start the poll-draw thread. Once at startup, if enabled.
 * Returns 1 on success, 0 if the target couldn't be found/patched. */
int  osd_install(void);

/* Stop/join the poll-draw thread. Call from module_stop before freeing. */
void osd_shutdown(void);

/* Draw mode: 0=auto (hook + poll fallback), 1=hook only, 2=poll only. */
void osd_set_draw_mode(int mode);

/* Show "Brightness: <level>" briefly. Safe from the display hook. */
void osd_notify(int level);
void osd_probe(int level, unsigned int unk1);  /* debug: raw firmware args */
void osd_note(const char *tag, int level);     /* debug: "<tag> L=<level>" */
void osd_debug(const char *event, int level, unsigned int unk1); /* DEBUG line */
void osd_message(const char *s);               /* one-off message (first-run credit) */

/* Current draw path as a short static string: "api-hook" or "fb-poll". */
const char *osd_draw_path_name(void);

/* 1 while the overlay is on screen. */
int osd_is_visible(void);

/* Colours (palette index), size 1x..4x, position 1=bottom 2=top. Before install. */
void osd_set_style(int text_colour, int bg_colour, int size, int position);

/* OSD word language (0-11, PSP_SYSTEMPARAM_LANGUAGE_*); non-Latin use word images. */
void osd_set_language(int lang);

extern volatile unsigned int osd_last_frame_us; /* time of last drawn frame (us) */

/* Tick the visibility timer (once per worker loop). */
void osd_tick(void);

/* Dump OSD counters to the log (worker context only). */
void osd_log_status(void);

#endif
