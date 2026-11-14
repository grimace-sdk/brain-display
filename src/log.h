/*
 * log.h - Debug log interface (brain_log, value logging, flush, enable gates).
 */
#ifndef BRAIN_LOG_H
#define BRAIN_LOG_H

void brain_log(const char *message);
void brain_log_value(const char *label, unsigned long value);
void brain_log_debug(const char *message);
void brain_log_set_suspended(int suspended);
void brain_log_set_debug(int enabled);
int brain_log_debug_active(void);
void brain_log_hold_file_io(unsigned long milliseconds);
void brain_log_flush(void);

#endif
