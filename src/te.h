#ifndef BRAIN_TE_H
#define BRAIN_TE_H

#include <windows.h>
#include <stdint.h>

int brain_te_init(void);
int brain_te_available(void);
int brain_te_wait(void);
void brain_te_recover(void);

void brain_te_arm(void);
void brain_te_disarm(void);
HANDLE brain_te_tick_handle(void);
uint32_t brain_te_count(void);
void brain_te_last(uint32_t *count, uint32_t *time_us);
uint32_t brain_te_period_us(void);

#endif
