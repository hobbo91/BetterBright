#ifndef LOG_H__
#define LOG_H__

/*
 * Opt-in diagnostic log (BetterBright.log), driven by debug_enable=2.
 * log_* push to an in-RAM ring (safe from ANY context, hooks included); the worker
 * calls log_drain() to write to disk. Off = cheap no-op.
 */

void log_set_path(const char *plugin_path);  /* derive <dir>/BetterBright.log */
void log_enable(int on);                      /* 1 = write file (debug>=2)    */
int  log_is_on(void);
void log_reset(void);                          /* truncate the file           */

/* ---- push a line into the ring (safe from ANY context) ---- */
void log_msg(const char *s);                   /* "<s>"               */
void log_kv(const char *label, int v);         /* "<label>=<dec>"     */
void log_kx(const char *label, unsigned int v);/* "<label>=0x<HEX>"   */
void log_ks(const char *label, const char *v); /* "<label>=<string>"  */
/* "event=<event> fwL=<fwL> plU=<plU> draw=<draw>" (draw may be NULL). */
void log_event(const char *event, int fwL, int plU, const char *draw);

/* Flush the ring to disk. SAFE CONTEXT ONLY (worker / module start/stop). */
void log_drain(void);

#endif
