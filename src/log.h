#ifndef LOG_H__
#define LOG_H__

/*
 * Opt-in diagnostic log (BetterBright.log, next to the plugin). Enabled with
 * `log=1` in BetterBright.ini. When off, every call here is a cheap no-op.
 *
 * IMPORTANT: only call these from a safe thread context (module_start or the
 * worker thread). Never from the display/brightness hooks - file I/O there can
 * deadlock. The hooks instead bump in-RAM counters that the worker logs.
 */

void log_set_path(const char *plugin_path);  /* derive <dir>/BetterBright.log */
void log_enable(int on);                      /* 1 = on (default off)          */
int  log_is_on(void);
void log_reset(void);                          /* truncate the file (fresh boot)*/

void log_msg(const char *s);                   /* "<s>"                         */
void log_kv(const char *label, int v);         /* "<label>=<decimal>"           */
void log_kx(const char *label, unsigned int v);/* "<label>=0x<HEX>"             */

#endif
