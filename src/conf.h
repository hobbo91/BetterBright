#ifndef CONFIG_H__
#define CONFIG_H__

/* one configured brightness step */
typedef struct {
	int level;
} Bright;

/* options read from BetterBright.ini (add new options here without touching the
 * ReadItem signature) */
typedef struct {
	int combo_mode;       /* 0 = off, 1 = L/R + Display button, 2 = L+R+Up/Down  */
	int hold_brightness;  /* 1 = keep screen fully on (no idle dim, no auto-off) */
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
