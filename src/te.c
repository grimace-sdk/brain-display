#include <windows.h>
#include <grimace/extras.h>
#include <stdint.h>

#include "board.h"
#include "te.h"
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

#define PINCTRL_PHYS_BASE 0x80018000u
#define PINCTRL_MAP_SIZE  0x2000u

#define PINCTRL_MUXSEL    0x100
#define PINCTRL_DOE       0xB00
#define PINCTRL_PIN2IRQ   0x1000
#define PINCTRL_IRQEN     0x1100
#define PINCTRL_IRQLEVEL  0x1200
#define PINCTRL_IRQPOL    0x1300
#define PINCTRL_IRQSTAT   0x1400

static volatile uint32_t *g_pinctrl;
static HANDLE g_event;
static DWORD g_sysintr;
static uint32_t g_pin_mask;
static uint32_t g_bank_offset;
static uint32_t g_timeout;
static int g_available;

static void reg_write(uint32_t byte_offset, uint32_t value) {
    g_pinctrl[byte_offset / 4] = value;
}

static uint32_t bank_reg(uint32_t base) {
    return base + g_bank_offset;
}

static void setup_pin(void) {
    const brain_board *board = &brain_current_board;

    uint32_t mux_index = (uint32_t)board->te_bank * 2 + (board->te_pin >= 16 ? 1 : 0);
    uint32_t mux_offset = PINCTRL_MUXSEL + mux_index * 0x10;
    uint32_t mux_shift = (uint32_t)(board->te_pin & 15) * 2;
    reg_write(mux_offset + 8, 3u << mux_shift);
    reg_write(mux_offset + 4, 3u << mux_shift);

    reg_write(bank_reg(PINCTRL_DOE) + 8, g_pin_mask);
    reg_write(bank_reg(PINCTRL_PIN2IRQ) + 4, g_pin_mask);
    reg_write(bank_reg(PINCTRL_IRQLEVEL) + 8, g_pin_mask);
    if (board->te_rising) {
        reg_write(bank_reg(PINCTRL_IRQPOL) + 4, g_pin_mask);
    } else {
        reg_write(bank_reg(PINCTRL_IRQPOL) + 8, g_pin_mask);
    }
    reg_write(bank_reg(PINCTRL_IRQSTAT) + 8, g_pin_mask);
    reg_write(bank_reg(PINCTRL_IRQEN) + 8, g_pin_mask);
}

int brain_te_init(void) {
    const brain_board *board = &brain_current_board;

    void *base = VirtualAlloc(NULL, PINCTRL_MAP_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (base == NULL) {
        brain_log("te: VirtualAlloc failed");
        return 0;
    }
    if (!VirtualCopy(base, (LPVOID)(PINCTRL_PHYS_BASE >> 8), PINCTRL_MAP_SIZE,
                     PAGE_READWRITE | PAGE_PHYSICAL | PAGE_NOCACHE)) {
        brain_log("te: VirtualCopy failed");
        return 0;
    }
    g_pinctrl = (volatile uint32_t *)base;

    g_pin_mask = 1u << board->te_pin;
    g_bank_offset = (uint32_t)board->te_bank * 0x10;
    g_timeout = (uint32_t)board->te_timeout_ms;

    setup_pin();

    DWORD irq = (DWORD)board->te_irq;
    g_sysintr = SYSINTR_UNDEFINED;
    if (!KernelIoControl(IOCTL_HAL_REQUEST_SYSINTR, &irq, sizeof(irq),
                         &g_sysintr, sizeof(g_sysintr), NULL)
        || g_sysintr == SYSINTR_UNDEFINED) {
        brain_log("te: request sysintr failed");
        return 0;
    }
    brain_log_value("te: sysintr", g_sysintr);

    g_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (g_event == NULL) {
        brain_log("te: CreateEvent failed");
        return 0;
    }
    if (!InterruptInitialize(g_sysintr, g_event, NULL, 0)) {
        brain_log("te: InterruptInitialize failed");
        CloseHandle(g_event);
        g_event = NULL;
        return 0;
    }

    g_available = 1;
    brain_log("te: ready");
    return 1;
}

int brain_te_available(void) {
    return g_available;
}

int brain_te_wait(void) {
    if (!g_available) {
        return 1;
    }
    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);
    for (int attempt = 0; attempt < 8; attempt++) {
        reg_write(bank_reg(PINCTRL_IRQSTAT) + 8, g_pin_mask);
        reg_write(bank_reg(PINCTRL_IRQEN) + 4, g_pin_mask);
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        DWORD result = WaitForSingleObject(g_event, g_timeout);
        QueryPerformanceCounter(&t1);
        reg_write(bank_reg(PINCTRL_IRQEN) + 8, g_pin_mask);
        reg_write(bank_reg(PINCTRL_IRQSTAT) + 8, g_pin_mask);
        InterruptDone(g_sysintr);
        if (result != WAIT_OBJECT_0) {
            return 0;
        }
        uint32_t us = qpf.QuadPart
            ? (uint32_t)((t1.QuadPart - t0.QuadPart) * 1000000 / qpf.QuadPart)
            : 2000;
        if (us >= 1000) {
            return 1;
        }
    }
    return 1;
}

void brain_te_recover(void) {
    if (!g_available) {
        return;
    }
    brain_log("te: recover re-arm pin");
    setup_pin();
}
