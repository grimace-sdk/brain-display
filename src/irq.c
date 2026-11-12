#include <windows.h>

#include "irq.h"
#include "log.h"

BOOL WINAPI KernelIoControl(DWORD code, LPVOID in_buffer, DWORD in_size,
                            LPVOID out_buffer, DWORD out_size, LPDWORD returned);
BOOL InterruptInitialize(DWORD id, HANDLE event, LPVOID data, DWORD data_size);
void InterruptDone(DWORD id);

#define CTL_CODE(device, function, method, access) \
    (((device) << 16) | ((access) << 14) | ((function) << 2) | (method))
#define FILE_DEVICE_HAL 0x00000101
#define IOCTL_HAL_REQUEST_SYSINTR CTL_CODE(FILE_DEVICE_HAL, 38, 0, 0)
#define SYSINTR_UNDEFINED 0

int brain_irq_init(brain_irq *irq, int oal_irq) {
    irq->ready = 0;
    irq->sysintr = SYSINTR_UNDEFINED;
    irq->event = NULL;

    DWORD number = (DWORD)oal_irq;
    if (!KernelIoControl(IOCTL_HAL_REQUEST_SYSINTR, &number, sizeof(number),
                         &irq->sysintr, sizeof(irq->sysintr), NULL)
        || irq->sysintr == SYSINTR_UNDEFINED) {
        brain_log_value("irq: request failed", (unsigned long)oal_irq);
        return 0;
    }
    irq->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (irq->event == NULL) {
        brain_log("irq: CreateEvent failed");
        return 0;
    }
    if (!InterruptInitialize(irq->sysintr, irq->event, NULL, 0)) {
        brain_log("irq: InterruptInitialize failed");
        CloseHandle(irq->event);
        irq->event = NULL;
        return 0;
    }
    irq->ready = 1;
    brain_log_value("irq: ready sysintr", irq->sysintr);
    return 1;
}

void brain_irq_done(brain_irq *irq) {
    if (irq->ready) {
        InterruptDone(irq->sysintr);
    }
}
