#ifndef LOG_H__
#define LOG_H__

/*
 * Opt-in diagnostic log (BetterBright.log, next to the plugin). Driven by
 * debug_enable=2 in BetterBright.ini. When off, every call here is a cheap no-op.
 *
 * HOW IT STAYS SAFE *AND* CAPTURES RUNTIME EVENTS
 * -----------------------------------------------
 * The old log did file I/O at the call site, so it could only be used from a safe
 * context (module_start / the worker). That meant nothing the plugin does at
 * runtime - presses, dim, wake, applies - could be logged, because those happen
 * inside the brightness/display hooks where file I/O is unsafe. So the log was
 * just the boot dump and looked like it "died" after startup.
 *
 * Now every log_* call only formats a line into an in-RAM ring buffer (pure
 * memory, no syscalls, briefly interrupts-off) - so it is safe to call from ANY
 * context, hooks included. The worker thread calls log_drain() once per loop to
 * flush the ring to the file from its safe context (the same mechanism that makes
 * SaveBrightness work at runtime). Lines keep their push-time timestamp, so order
 * and timing are correct even though the write happens a moment later.
 */

void log_set_path(const char *plugin_path);  /* derive <dir>/BetterBright.log     */
void log_enable(int on);                      /* 1 = write file (debug_enable>=2)  */
int  log_is_on(void);
void log_reset(void);                          /* truncate the file (fresh boot)   */

/* ---- push a line into the ring (safe from ANY context, hooks included) -------- */
void log_msg(const char *s);                   /* "<s>"                            */
void log_kv(const char *label, int v);         /* "<label>=<decimal>"              */
void log_kx(const char *label, unsigned int v);/* "<label>=0x<HEX>"                */
void log_ks(const char *label, const char *v); /* "<label>=<string>"               */

/* Rich brightness-event line: "event=<event> fwL=<fwL> plU=<plU> draw=<draw>".
 * draw may be NULL to omit it. fwL = firmware/native level, plU = our level. */
void log_event(const char *event, int fwL, int plU, const char *draw);

/* Flush the ring to disk. Call ONLY from a safe context (worker / module_start /
 * module_stop) - it does the actual sceIo* file I/O. No-op when logging is off or
 * the ring is empty. */
void log_drain(void);

#endif
