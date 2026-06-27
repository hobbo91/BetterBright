#ifndef CONFIG_H__
#define CONFIG_H__

/* one configured brightness step */
typedef struct {
	int level;
} Bright;

/* options read from BetterBright.ini (add new options here without touching the
 * ReadItem signature) */
typedef struct {
	int combo_mode;            /* 0 = off, 1 = L/R + Display button, 2 = L+R+Up/Down */
	int dim_level;             /* -1 = AUTO (2nd-lowest ini value); else 0-100       */
	int keep_display_on;       /* 1 = never dim AND never auto-off (screen stays on)  */
	int disable_sleep;         /* 1 = prevent the auto-sleep (idle suspend) timer    */
	int osd_enable;            /* 1 = show "Display Brightness: NN" on each change    */
	int debug_enable;          /* 1 = verbose log + DEBUG line on the OSD            */
	int osd_bg_colour;         /* 1=black 2=white 3=red 4=green 5=blue (bg default 1) */
	int osd_text_colour;       /* 1=black 2=white 3=red 4=green 5=blue (text def 2)   */
	int osd_size;              /* 1 = normal, 2 = large (2x)                          */
	int osd_position;          /* 1 = bottom, 2 = top                                 */
	int osd_draw_mode;         /* 0 = auto (hook, poll fallback), 1 = hook only,      */
	                           /* 2 = poll only (draw into the live framebuffer)      */
} BrightSettings;

/* paths */
int GetConfigPath(char *buf);   /* -> <plugin dir>/BetterBright.ini */
int GetDataPath(char *buf);     /* -> <plugin dir>/BetterBright.dat */

/* config file */
int CountItem(char *file);
int ReadItem(const char *file, Bright *buf, BrightSettings *settings);

/* persistence (BetterBright.dat) */
int SaveBrightness(const char *file, int level, int index);
int LoadBrightness(const char *file, int *level, int *index);

/* misc (kept from original) */
void *malloc_p(SceSize size);

#endif
