#ifndef CONFIG_H__
#define CONFIG_H__

/* one brightness step */
typedef struct {
	int level;
} Bright;

/* options from BetterBright.ini (see the .ini for full descriptions) */
typedef struct {
	int combo_mode;            /* 0=off 1=L/R+Display 2=L+R+Up/Down */
	int dim_level;             /* -1=AUTO else 0-100                */
	int keep_display_on;       /* 1=never dim/off                   */
	int disable_sleep;         /* 1=never auto-sleep                */
	int osd_enable;            /* 1=show "Brightness: NN"           */
	int debug_enable;          /* 0=off 1=OSD line 2=+log file      */
	int osd_bg_colour;         /* palette index (default 1 black)   */
	int osd_text_colour;       /* palette index (default 2 white)   */
	int osd_size;              /* 1x..4x                            */
	int osd_position;          /* 1=bottom 2=top                    */
	int osd_draw_mode;         /* 0=auto 1=hook-only 2=poll-only    */
	int detect_locale;         /* 1=system language 0=English       */
	int sync_fw_level;         /* 1=sync firmware (impose) level    */
	int oem_brightness_levels; /* empty list: 1=4 stock L steps, 0=wide defaults */
} BrightSettings;

/* paths -> <plugin dir>/BetterBright.{ini,dat} */
int GetConfigPath(char *buf);
int GetDataPath(char *buf);

/* config file */
int CountItem(char *file);
int ReadItem(const char *file, Bright *buf, BrightSettings *settings);

/* persistence (BetterBright.dat) */
int SaveBrightness(const char *file, int level, int index);
int LoadBrightness(const char *file, int *level, int *index, char *version);

void *malloc_p(SceSize size);

#endif
