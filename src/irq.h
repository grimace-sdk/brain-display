#ifndef BRAIN_IRQ_H
#define BRAIN_IRQ_H

#include <windows.h>

typedef struct {
    int ready;
    DWORD sysintr;
    HANDLE event;
} brain_irq;

int brain_irq_init(brain_irq *irq, int oal_irq);
void brain_irq_done(brain_irq *irq);

#endif
