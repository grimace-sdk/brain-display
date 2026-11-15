/*
 * panel.c - Command-mode panel over the LCDIF: pixel clock setup, window
 * addressing, starting and waiting on TE-paced transfers, and stuck-transfer
 * recovery.
 */
#include <windows.h>
#include <grimace/extras.h>

#include "board.h"
#include "panel.h"
#include "irq.h"
#include "log.h"

#define LCDIF_IRQ 38

#define LCDIF_PHYS_BASE   0x80030000u
#define LCDIF_MAP_SIZE    0x2000u

#define CLKCTRL_PHYS_BASE 0x80040000u
#define CLKCTRL_MAP_SIZE  0x2000u
#define REG_DIS_LCDIF     0x120
#define REG_FRAC1         0x1C0
#define REG_FRAC1_CLR     0x1C8
#define REG_CLKSEQ        0x1D0
#define REG_CLKSEQ_CLR    0x1D8
#define CLKCTRL_CLKGATE   (1u << 31)
#define DIS_LCDIF_BUSY    (1u << 29)
#define DIS_LCDIF_DIV_MASK 0x3Fu
#define DIS_LCDIF_DIV     9u
#define FRAC1_CLKGATEPIX  (1u << 7)
#define FRAC1_PIXSTABLE   (1u << 6)
#define CLKSEQ_BYPASS_DIS_LCDIF (1u << 14)

#define PINCTRL_PHYS_BASE 0x80018000u
#define PINCTRL_MAP_SIZE  0x2000u
#define PINCTRL_MUXSEL2   0x120
#define PINCTRL_MUXSEL3   0x130
#define PINCTRL_SET       0x004
#define PINCTRL_CLR       0x008
#define LCD_MUXSEL2_MASK  0xFFFFFFFFu
#define LCD_MUXSEL3_MASK  0x00FF0000u

/* Panel control GPIOs that deep sleep drops low. Bank 0 pins 26-27 are the panel
   power sequence and bank 1 pins 16-23 (LCD_D16-D23 as GPIO) are panel control,
   both driven high by EBOOT (panel-handoff.md). Bank n DOUT is 0x700 + n*0x10,
   DOE is 0xB00 + n*0x10. */
#define PINCTRL_DOUT      0x700
#define PINCTRL_DOE       0xB00
#define PANEL_GPIO_BANK0  0
#define PANEL_GPIO_BANK1  1
#define PANEL_GPIO_BANK0_MASK 0x0C000000u
#define PANEL_GPIO_BANK1_MASK 0x00FF0000u

/* Panel reset is GPIO 4_16, an active-low line EBOOT drives high in normal use
   (panel-handoff.md). Deep sleep drops it low, holding the panel in reset, so a
   deep-sleep resume needs a reset pulse and a full re-init. On the PW-SH2 this is
   not the TE pin (TE is 3_30), so no TE coordination is needed. Bank 4: DOUT
   0x740, DOE 0xB40, MUXSEL9 0x190 (pin 16 at shift 0, GPIO = mux 3). */
#define PANEL_RESET_MUXSEL 0x190
#define PANEL_RESET_DOUT   0x740
#define PANEL_RESET_DOE    0xB40
#define PANEL_RESET_BIT    0x00010000u
#define PANEL_RESET_MUX_GPIO 0x3u

#define REG_CTRL          0x000
#define REG_CTRL_SET      0x004
#define REG_CTRL_CLR      0x008
#define REG_CTRL1         0x010
#define REG_CTRL1_SET     0x014
#define REG_CTRL1_CLR     0x018
#define REG_CTRL2         0x020
#define REG_TRANSFER_COUNT 0x030
#define REG_CUR_BUF       0x040
#define REG_NEXT_BUF      0x050
#define REG_TIMING        0x060
#define REG_DATA          0x180
#define REG_STAT          0x1B0

#define CTRL_RUN          (1u << 0)
#define CTRL_LCDIF_MASTER (1u << 5)
#define CTRL_DATA_SELECT  (1u << 16)
#define CTRL_CLKGATE      (1u << 30)
#define CTRL_SFTRST       (1u << 31)
#define CTRL1_BYTE_PACKING_SHIFT 16
#define CTRL1_CUR_FRAME_DONE_IRQ (1u << 9)
#define CTRL1_CUR_FRAME_DONE_IRQ_EN (1u << 13)
#define CTRL1_UNDERFLOW_IRQ (1u << 10)
#define CTRL1_FIFO_CLEAR  (1u << 21)

#define DCS_SET_TEAR_ON 0x35

#define DCS_CASET      0x2A
#define DCS_PASET      0x2B
#define DCS_RAMWR      0x2C
#define DCS_MADCTL     0x36
#define DCS_COLMOD     0x3A
#define DCS_SLEEP_IN   0x10
#define DCS_SLEEP_OUT  0x11
#define DCS_DISPLAY_OFF 0x28
#define DCS_DISPLAY_ON 0x29

static uint32_t g_saved_ctrl;
static uint32_t g_saved_ctrl1;
static uint32_t g_saved_ctrl2;
static uint32_t g_saved_timing;
static uint32_t g_saved_muxsel2;
static uint32_t g_saved_muxsel3;
static uint32_t g_saved_dout0;
static uint32_t g_saved_dout1;
static uint32_t g_saved_doe0;
static uint32_t g_saved_doe1;

static void panel_configure(void);
#if defined(BRAIN_BOARD_pwsh2)
static void panel_reset_pulse(void);
static void panel_ili9805_init(void);
#endif

static volatile uint32_t *g_lcdif;
static volatile uint32_t *g_clkctrl;
static volatile uint32_t *g_pinctrl;

static uint32_t reg_read(uint32_t offset) {
    return g_lcdif[offset / 4];
}

static void *map_physical(uint32_t phys, uint32_t size) {
    void *base = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
    if (base == NULL) {
        return NULL;
    }
    if (!VirtualCopy(base, (LPVOID)(phys >> 8), size, PAGE_READWRITE | PAGE_PHYSICAL | PAGE_NOCACHE)) {
        return NULL;
    }
    return base;
}

static int clock_wait(uint32_t offset, uint32_t mask, int want_set) {
    uint32_t start = GetTickCount();
    for (;;) {
        uint32_t set = (g_clkctrl[offset / 4] & mask) != 0;
        if (set == (uint32_t)want_set) {
            return 1;
        }
        if (GetTickCount() - start > 10) {
            return 0;
        }
    }
}

static void clock_enable(void) {
    if (g_clkctrl == NULL) {
        return;
    }
    brain_log_value("clock: DIS_LCDIF", g_clkctrl[REG_DIS_LCDIF / 4]);
    brain_log_value("clock: FRAC1", g_clkctrl[REG_FRAC1 / 4]);
    brain_log_value("clock: CLKSEQ", g_clkctrl[REG_CLKSEQ / 4]);

    uint32_t div = g_clkctrl[REG_DIS_LCDIF / 4];
    div &= ~CLKCTRL_CLKGATE;
    div &= ~DIS_LCDIF_DIV_MASK;
    div |= DIS_LCDIF_DIV;
    g_clkctrl[REG_DIS_LCDIF / 4] = div;
    clock_wait(REG_DIS_LCDIF, DIS_LCDIF_BUSY, 0);

    g_clkctrl[REG_FRAC1_CLR / 4] = FRAC1_CLKGATEPIX;
    clock_wait(REG_FRAC1, FRAC1_PIXSTABLE, 1);

    g_clkctrl[REG_CLKSEQ_CLR / 4] = CLKSEQ_BYPASS_DIS_LCDIF;

    brain_log_value("clock: DIS_LCDIF after", g_clkctrl[REG_DIS_LCDIF / 4]);
    brain_log_value("clock: FRAC1 after", g_clkctrl[REG_FRAC1 / 4]);
    brain_log_value("clock: CLKSEQ after", g_clkctrl[REG_CLKSEQ / 4]);
}

static void reg_write(uint32_t offset, uint32_t value) {
    g_lcdif[offset / 4] = value;
}

static void pio_byte(int data_select, uint32_t value) {
    reg_write(REG_CTRL_CLR, CTRL_RUN);
    reg_write(REG_CTRL_CLR, CTRL_LCDIF_MASTER);
    reg_write(REG_CTRL1_CLR, 0xFu << CTRL1_BYTE_PACKING_SHIFT);
    reg_write(REG_CTRL1_SET, 0x3u << CTRL1_BYTE_PACKING_SHIFT);
    reg_write(REG_TRANSFER_COUNT, (1u << 16) | 1u);
    if (data_select) {
        reg_write(REG_CTRL_SET, CTRL_DATA_SELECT);
    } else {
        reg_write(REG_CTRL_CLR, CTRL_DATA_SELECT);
    }
    reg_write(REG_CTRL_SET, CTRL_RUN);
    reg_write(REG_DATA, value & 0xFF);
    uint32_t start = GetTickCount();
    while ((reg_read(REG_CTRL) & CTRL_RUN) && (GetTickCount() - start) < 10) {
    }
    reg_write(REG_CTRL_CLR, CTRL_RUN);
}

static void panel_command(uint32_t command) {
    pio_byte(0, command);
}

static void panel_param(uint32_t param) {
    pio_byte(1, param);
}

void brain_panel_command_raw(const uint8_t *bytes, uint32_t length) {
    if (length == 0) {
        return;
    }
    pio_byte(0, bytes[0]);
    for (uint32_t i = 1; i < length; i++) {
        pio_byte(1, bytes[i]);
    }
}

void brain_panel_set_window(int32_t col_start, int32_t col_count, int32_t page_start, int32_t page_count) {
    int32_t col_end = col_start + col_count - 1;
    int32_t page_end = page_start + page_count - 1;

    panel_command(DCS_CASET);
    panel_param((uint32_t)col_start >> 8);
    panel_param((uint32_t)col_start & 0xFF);
    panel_param((uint32_t)col_end >> 8);
    panel_param((uint32_t)col_end & 0xFF);

    panel_command(DCS_PASET);
    panel_param((uint32_t)page_start >> 8);
    panel_param((uint32_t)page_start & 0xFF);
    panel_param((uint32_t)page_end >> 8);
    panel_param((uint32_t)page_end & 0xFF);

    panel_command(DCS_RAMWR);
}

static brain_irq g_lcdif_irq;

static int lcdif_transfer_done(void) {
    return (reg_read(REG_CTRL1) & CTRL1_CUR_FRAME_DONE_IRQ) ||
           !(reg_read(REG_CTRL) & CTRL_RUN);
}

static int lcdif_poll_done(uint32_t timeout_ms) {
    uint32_t start = GetTickCount();
    while (!lcdif_transfer_done()) {
        if (GetTickCount() - start > timeout_ms) {
            return 0;
        }
    }
    return 1;
}

void brain_panel_transfer_start(uint32_t phys, int32_t pixels_per_row, int32_t rows) {
    reg_write(REG_CTRL_CLR, CTRL_RUN);
    reg_write(REG_CTRL1_CLR, CTRL1_CUR_FRAME_DONE_IRQ);
    reg_write(REG_CTRL1_CLR, 0xFu << CTRL1_BYTE_PACKING_SHIFT);
    reg_write(REG_CTRL1_SET, 0xFu << CTRL1_BYTE_PACKING_SHIFT);
    reg_write(REG_CUR_BUF, phys);
    reg_write(REG_NEXT_BUF, phys);
    reg_write(REG_TRANSFER_COUNT, ((uint32_t)rows << 16) | (uint32_t)pixels_per_row);
    reg_write(REG_CTRL_SET, CTRL_DATA_SELECT | CTRL_LCDIF_MASTER);
    if (g_lcdif_irq.ready) {
        reg_write(REG_CTRL1_SET, CTRL1_CUR_FRAME_DONE_IRQ_EN);
    }
    reg_write(REG_CTRL_SET, CTRL_RUN);
}

int brain_panel_transfer_wait(void) {
    int ok;
    if (g_lcdif_irq.ready) {
        WaitForSingleObject(g_lcdif_irq.event, 150);
        ok = lcdif_transfer_done();
        reg_write(REG_CTRL1_CLR, CTRL1_CUR_FRAME_DONE_IRQ);
        reg_write(REG_CTRL1_CLR, CTRL1_CUR_FRAME_DONE_IRQ_EN);
        brain_irq_done(&g_lcdif_irq);
    } else {
        ok = lcdif_poll_done(150);
    }
    reg_write(REG_CTRL_CLR, CTRL_RUN);
    reg_write(REG_CTRL1_CLR, CTRL1_CUR_FRAME_DONE_IRQ);

    if (!ok) {
        brain_log_value("xfer TIMEOUT CTRL", reg_read(REG_CTRL));
        brain_log_value("xfer TIMEOUT CTRL1", reg_read(REG_CTRL1));
        brain_log_value("xfer TIMEOUT STAT", reg_read(REG_STAT));
    }
    return ok;
}

int brain_panel_transfer(uint32_t phys, int32_t pixels_per_row, int32_t rows) {
    brain_panel_transfer_start(phys, pixels_per_row, rows);
    return brain_panel_transfer_wait();
}

uint32_t brain_panel_reg_ctrl(void) {
    return reg_read(REG_CTRL);
}

uint32_t brain_panel_reg_ctrl1(void) {
    return reg_read(REG_CTRL1);
}

uint32_t brain_panel_reg_stat(void) {
    return reg_read(REG_STAT);
}

static void wait_bit_clear(uint32_t offset, uint32_t mask) {
    uint32_t start = GetTickCount();
    while ((reg_read(offset) & mask) && (GetTickCount() - start) < 10) {
    }
}

static void wait_bit_set(uint32_t offset, uint32_t mask) {
    uint32_t start = GetTickCount();
    while (!(reg_read(offset) & mask) && (GetTickCount() - start) < 10) {
    }
}

static void lcdif_soft_reset(void) {
    reg_write(REG_CTRL_CLR, CTRL_SFTRST);
    wait_bit_clear(REG_CTRL, CTRL_SFTRST);
    reg_write(REG_CTRL_CLR, CTRL_CLKGATE);
    reg_write(REG_CTRL_SET, CTRL_SFTRST);
    wait_bit_set(REG_CTRL, CTRL_CLKGATE);
    reg_write(REG_CTRL_CLR, CTRL_SFTRST);
    wait_bit_clear(REG_CTRL, CTRL_SFTRST);
    reg_write(REG_CTRL_CLR, CTRL_CLKGATE);
    wait_bit_clear(REG_CTRL, CTRL_CLKGATE);
}

void brain_panel_recover(uint32_t level) {
    if (level <= 1) {
        brain_log("panel: recover L1 fifo clear");
        reg_write(REG_CTRL_CLR, CTRL_RUN);
        reg_write(REG_CTRL1_SET, CTRL1_FIFO_CLEAR);
        reg_write(REG_CTRL1_CLR, CTRL1_FIFO_CLEAR);
        reg_write(REG_CTRL1_CLR, CTRL1_UNDERFLOW_IRQ);
    } else if (level == 2) {
        brain_log("panel: recover L2 soft reset");
        lcdif_soft_reset();
        if (g_clkctrl != NULL) {
            clock_enable();
        }
        reg_write(REG_CTRL, g_saved_ctrl);
        reg_write(REG_CTRL1, g_saved_ctrl1);
        reg_write(REG_CTRL2, g_saved_ctrl2);
        reg_write(REG_TIMING, g_saved_timing);
    } else {
        brain_log("panel: recover L3 panel reinit");
#if defined(BRAIN_BOARD_pwsh2)
        panel_reset_pulse();
        panel_ili9805_init();
        panel_command(DCS_SLEEP_OUT);
        Sleep(120);
        panel_command(DCS_DISPLAY_ON);
#else
        panel_command(DCS_SLEEP_OUT);
        Sleep(120);
        panel_configure();
        panel_command(DCS_DISPLAY_ON);
#endif
    }
}

int brain_panel_init(void) {
    void *base = VirtualAlloc(NULL, LCDIF_MAP_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (base == NULL) {
        brain_log("panel: VirtualAlloc failed");
        return 0;
    }
    if (!VirtualCopy(base, (LPVOID)(LCDIF_PHYS_BASE >> 8), LCDIF_MAP_SIZE,
                     PAGE_READWRITE | PAGE_PHYSICAL | PAGE_NOCACHE)) {
        brain_log("panel: VirtualCopy failed");
        return 0;
    }
    g_lcdif = (volatile uint32_t *)base;
    brain_log_value("panel: LCDIF CTRL", reg_read(REG_CTRL));
    brain_log_value("panel: LCDIF CTRL1", reg_read(REG_CTRL1));
    brain_log_value("panel: LCDIF TIMING", reg_read(REG_TIMING));

    g_clkctrl = (volatile uint32_t *)map_physical(CLKCTRL_PHYS_BASE, CLKCTRL_MAP_SIZE);
    if (g_clkctrl == NULL) {
        brain_log("panel: CLKCTRL map failed");
    } else {
        clock_enable();
    }

    g_pinctrl = (volatile uint32_t *)map_physical(PINCTRL_PHYS_BASE, PINCTRL_MAP_SIZE);
    if (g_pinctrl == NULL) {
        brain_log("panel: PINCTRL map failed");
    } else {
        g_saved_muxsel2 = g_pinctrl[PINCTRL_MUXSEL2 / 4];
        g_saved_muxsel3 = g_pinctrl[PINCTRL_MUXSEL3 / 4];
        g_saved_dout0 = g_pinctrl[(PINCTRL_DOUT + PANEL_GPIO_BANK0 * 0x10) / 4];
        g_saved_dout1 = g_pinctrl[(PINCTRL_DOUT + PANEL_GPIO_BANK1 * 0x10) / 4];
        g_saved_doe0 = g_pinctrl[(PINCTRL_DOE + PANEL_GPIO_BANK0 * 0x10) / 4];
        g_saved_doe1 = g_pinctrl[(PINCTRL_DOE + PANEL_GPIO_BANK1 * 0x10) / 4];
        brain_log_value("panel: init MUXSEL2", g_saved_muxsel2);
        brain_log_value("panel: init MUXSEL3", g_saved_muxsel3);
        brain_log_value("panel: init DOUT0", g_saved_dout0);
        brain_log_value("panel: init DOUT1", g_saved_dout1);
    }

    g_saved_ctrl = reg_read(REG_CTRL);
    g_saved_ctrl1 = reg_read(REG_CTRL1);
    g_saved_ctrl2 = reg_read(REG_CTRL2);
    g_saved_timing = reg_read(REG_TIMING);

    panel_configure();
    brain_irq_init(&g_lcdif_irq, LCDIF_IRQ);
    brain_log("panel: init done");
    return 1;
}

static void panel_configure(void) {
#if defined(BRAIN_BOARD_pwa7300)
    panel_command(DCS_MADCTL);
    panel_param(0x08);
#endif
    panel_command(DCS_COLMOD);
    panel_param(0x55);
    panel_command(DCS_SET_TEAR_ON);
    panel_param(0x00);
}

void brain_panel_suspend(void) {
    brain_log("panel: suspend enter");
    brain_log_value("panel: suspend CTRL", reg_read(REG_CTRL));
    brain_log_value("panel: suspend STAT", reg_read(REG_STAT));
    brain_log("panel: suspend DISPLAY_OFF");
    panel_command(DCS_DISPLAY_OFF);
    brain_log("panel: suspend SLEEP_IN");
    panel_command(DCS_SLEEP_IN);
    brain_log("panel: suspend done");
}

/*
 * Deep sleep parks the LCD bus and control strobes back to GPIO. The LCDIF
 * function is mux value 0 on these bank-1 pins, so restoring it is a clear of
 * the owned fields: all of MUXSEL2 (LCD_D00-D15) and MUXSEL3 bits 16-23 (pins
 * 24-27, the RD/WR/RS/CS strobes). Using the CLR register leaves the other
 * pins in MUXSEL3 untouched, and the clear is a no-op after a short sleep.
 */
static void lcd_pinmux_restore(void) {
    if (g_pinctrl == NULL) {
        return;
    }
    brain_log_value("panel: resume MUXSEL2", g_pinctrl[PINCTRL_MUXSEL2 / 4]);
    brain_log_value("panel: resume MUXSEL3", g_pinctrl[PINCTRL_MUXSEL3 / 4]);
    g_pinctrl[(PINCTRL_MUXSEL2 + PINCTRL_CLR) / 4] = LCD_MUXSEL2_MASK;
    g_pinctrl[(PINCTRL_MUXSEL3 + PINCTRL_CLR) / 4] = LCD_MUXSEL3_MASK;
    brain_log("panel: resume LCD pinmux restored");
}

static void pinctrl_restore_bits(uint32_t reg_base, uint32_t saved, uint32_t mask) {
    uint32_t set_bits = saved & mask;
    uint32_t clear_bits = ~saved & mask;
    if (set_bits != 0) {
        g_pinctrl[(reg_base + PINCTRL_SET) / 4] = set_bits;
    }
    if (clear_bits != 0) {
        g_pinctrl[(reg_base + PINCTRL_CLR) / 4] = clear_bits;
    }
}

/*
 * Deep sleep drops the panel control and power GPIOs low: bank 0 pins 26-27 (the
 * panel power sequence) and bank 1 pins 16-23 (panel control). Re-drive them to
 * the state captured at init, value (DOUT) before enable (DOE), before the LCDIF
 * restore so the panel is powered. A no-op after a short sleep, when the pins are
 * already set. See panel-handoff.md for the EBOOT provenance.
 */
static void panel_gpio_restore(void) {
    if (g_pinctrl == NULL) {
        return;
    }
    brain_log_value("panel: resume DOUT0", g_pinctrl[(PINCTRL_DOUT + PANEL_GPIO_BANK0 * 0x10) / 4]);
    brain_log_value("panel: resume DOUT1", g_pinctrl[(PINCTRL_DOUT + PANEL_GPIO_BANK1 * 0x10) / 4]);
    pinctrl_restore_bits(PINCTRL_DOUT + PANEL_GPIO_BANK0 * 0x10, g_saved_dout0, PANEL_GPIO_BANK0_MASK);
    pinctrl_restore_bits(PINCTRL_DOUT + PANEL_GPIO_BANK1 * 0x10, g_saved_dout1, PANEL_GPIO_BANK1_MASK);
    pinctrl_restore_bits(PINCTRL_DOE + PANEL_GPIO_BANK0 * 0x10, g_saved_doe0, PANEL_GPIO_BANK0_MASK);
    pinctrl_restore_bits(PINCTRL_DOE + PANEL_GPIO_BANK1 * 0x10, g_saved_doe1, PANEL_GPIO_BANK1_MASK);
    brain_log("panel: resume panel GPIO restored");
}

#if defined(BRAIN_BOARD_pwsh2)
/* Pulse the panel reset line (GPIO 4_16, active low) with EBOOT's timing. Deep
   sleep leaves it low; drive it high, assert low, release high, 15 ms each. */
static void panel_reset_pulse(void) {
    if (g_pinctrl == NULL) {
        return;
    }
    g_pinctrl[(PANEL_RESET_MUXSEL + PINCTRL_SET) / 4] = PANEL_RESET_MUX_GPIO;
    g_pinctrl[(PANEL_RESET_DOE + PINCTRL_SET) / 4] = PANEL_RESET_BIT;
    g_pinctrl[(PANEL_RESET_DOUT + PINCTRL_SET) / 4] = PANEL_RESET_BIT;
    Sleep(15);
    g_pinctrl[(PANEL_RESET_DOUT + PINCTRL_CLR) / 4] = PANEL_RESET_BIT;
    Sleep(15);
    g_pinctrl[(PANEL_RESET_DOUT + PINCTRL_SET) / 4] = PANEL_RESET_BIT;
    Sleep(15);
    brain_log("panel: resume reset pulse");
}

/* Full ILI9805 controller init, ported from the EBOOT bring-up in
   panel-handoff.md (static analysis of Sharp's EBOOT, a user-approved source).
   Used after a deep-sleep reset, when the panel has lost its configuration.
   Window addressing and the frame follow from the refresh push, so CASET/PASET
   and the boot splash are omitted. SLEEP_OUT and display on are sent by the
   caller and the power handler, as on the normal resume path. */
static void panel_ili9805_init(void) {
    panel_command(0xFF); panel_param(0xFF); panel_param(0x98); panel_param(0x05);
    panel_command(0xFD); panel_param(0x03); panel_param(0x13); panel_param(0x44); panel_param(0x00);
    panel_command(0xF8);
    panel_param(0x18); panel_param(0x02); panel_param(0x02); panel_param(0x18); panel_param(0x02);
    panel_param(0x02); panel_param(0x30); panel_param(0x01); panel_param(0x01); panel_param(0x30);
    panel_param(0x01); panel_param(0x01); panel_param(0x30); panel_param(0x01); panel_param(0x01);
    panel_command(0xB8); panel_param(0x72);
    panel_command(0xF1); panel_param(0x00);
    panel_command(0xF2); panel_param(0x00); panel_param(0x58); panel_param(0x40);
    panel_command(0xFC); panel_param(0x04); panel_param(0x0F); panel_param(0x01);
    panel_command(0xEB); panel_param(0x08); panel_param(0x0F);
    panel_command(0xE0);
    panel_param(0x0A); panel_param(0x23); panel_param(0x35); panel_param(0x15); panel_param(0x13);
    panel_param(0x16); panel_param(0x0A); panel_param(0x06); panel_param(0x03); panel_param(0x06);
    panel_param(0x05); panel_param(0x0A); panel_param(0x08); panel_param(0x23); panel_param(0x1A);
    panel_param(0x00);
    panel_command(0xE1);
    panel_param(0x0A); panel_param(0x23); panel_param(0x28); panel_param(0x10); panel_param(0x11);
    panel_param(0x16); panel_param(0x0B); panel_param(0x0A); panel_param(0x02); panel_param(0x05);
    panel_param(0x04); panel_param(0x0A); panel_param(0x08); panel_param(0x1D); panel_param(0x1A);
    panel_param(0x00);
    panel_command(0xC1); panel_param(0x13); panel_param(0x28); panel_param(0x08); panel_param(0x26);
    panel_command(0xC7); panel_param(0x90);
    panel_command(0xB1); panel_param(0x00); panel_param(0x12); panel_param(0x14);
    panel_command(0xB4); panel_param(0x02);
    panel_command(0xBB); panel_param(0x14); panel_param(0x55);
    panel_command(DCS_MADCTL); panel_param(0x48);
    panel_command(DCS_COLMOD); panel_param(0x55);
    panel_command(0x21);
    panel_command(0xB6); panel_param(0x01); panel_param(0x80); panel_param(0x8F);
    panel_command(0x44); panel_param(0x00); panel_param(0x00);
    panel_command(DCS_SET_TEAR_ON); panel_param(0x00);
    brain_log("panel: resume ILI9805 init");
}
#endif

void brain_panel_resume_prepare(void) {
    uint32_t ctrl = reg_read(REG_CTRL);
    brain_log("panel: resume enter");
    brain_log_value("panel: resume CTRL", ctrl);
    brain_log_value("panel: resume STAT", reg_read(REG_STAT));
    lcd_pinmux_restore();
    panel_gpio_restore();
    if (ctrl & (CTRL_SFTRST | CTRL_CLKGATE)) {
        lcdif_soft_reset();
        brain_log("panel: resume LCDIF out of reset");
    }
    reg_write(REG_CTRL, g_saved_ctrl);
    reg_write(REG_CTRL1, g_saved_ctrl1);
    reg_write(REG_CTRL2, g_saved_ctrl2);
    reg_write(REG_TIMING, g_saved_timing);
    brain_log("panel: resume regs restored");
    if (g_clkctrl != NULL) {
        clock_enable();
    }
#if defined(BRAIN_BOARD_pwsh2)
    if (ctrl & CTRL_SFTRST) {
        brain_log("panel: resume full re-init");
        panel_reset_pulse();
        panel_ili9805_init();
        panel_command(DCS_SLEEP_OUT);
        Sleep(120);
    } else {
        brain_log("panel: resume SLEEP_OUT");
        panel_command(DCS_SLEEP_OUT);
        Sleep(120);
        panel_configure();
    }
#else
    brain_log("panel: resume SLEEP_OUT");
    panel_command(DCS_SLEEP_OUT);
    Sleep(120);
    panel_configure();
#endif
    brain_log_value("panel: resume done CTRL", reg_read(REG_CTRL));
    brain_log_value("panel: resume done STAT", reg_read(REG_STAT));
}

void brain_panel_display_on(void) {
    brain_log("panel: display_on enter");
    panel_command(DCS_DISPLAY_ON);
    brain_log("panel: display_on done");
}

/*
 * Apply night-light warmth through the panel's own gamma gain. The schedule and
 * strength maths are board-independent (nightlight.c); only the register write
 * differs per panel. The gain values are 6-bit, red held at maximum.
 *
 * PW-A7300: the panel's per-colour gamma tables are E2/E3 (green) and E4/E5
 * (blue), each a command plus nine data bytes, the two drive polarities of a
 * colour. Data byte 1 (offset 2 below) is the 6-bit gain; set it from the
 * computed value and resend both polarities of a colour together. The table
 * contents are the panel's own gamma, from our colourtune gamma tool.
 *
 * PW-SH2 (ILI9805): no-op. The ILI9805 has no per-colour gamma stage (probed on
 * device, see docs/plan.md M10), so night light is not offered on the SH2 - the
 * escape is not advertised there and this does nothing.
 */
#if defined(BRAIN_BOARD_pwa7300)
static void send_gamma_table(const uint8_t *table, uint32_t gain) {
    panel_command(table[0]);
    for (int i = 1; i < 10; i++) {
        panel_param(i == 2 ? (gain & 0x3Fu) : table[i]);
    }
}

void brain_panel_night_light(uint32_t green_gain, uint32_t blue_gain) {
    static const uint8_t green_even[10] = {0xE2, 0x24, 0x29, 0x14, 0x1C, 0x67, 0xDD, 0xDD, 0x97, 0x0B};
    static const uint8_t green_odd[10]  = {0xE3, 0x24, 0x29, 0x14, 0x1C, 0x67, 0xDD, 0xDD, 0x97, 0x0A};
    static const uint8_t blue_even[10]  = {0xE4, 0x24, 0x2A, 0x15, 0x1A, 0x99, 0xDD, 0xED, 0xA6, 0x09};
    static const uint8_t blue_odd[10]   = {0xE5, 0x24, 0x2A, 0x15, 0x1A, 0x88, 0xDD, 0xDD, 0x97, 0x0C};
    brain_log_value("night: green gain", green_gain);
    brain_log_value("night: blue gain", blue_gain);
    send_gamma_table(green_even, green_gain);
    send_gamma_table(green_odd, green_gain);
    send_gamma_table(blue_even, blue_gain);
    send_gamma_table(blue_odd, blue_gain);
}
#else
void brain_panel_night_light(uint32_t green_gain, uint32_t blue_gain) {
    (void)green_gain;
    (void)blue_gain;
}
#endif
