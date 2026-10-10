/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Modified for Modular-1 (morgulbrut). */
/* Boot and main loop, reduced from Felucca's: WDT, boot-loop guard, fatal vectors, then LCD, input (TIMER5
 * IRQ, 10 kHz), audio (ALNK0 IRQ), USB. Kept so the device can always be re-flashed: the boot-loop guard
 * (two failed boots -> UBOOT), OCT- + OCT+ held 5 s -> UBOOT, the OTA command and the UBOOT SysEx. */
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];

#define C_BG 0x0000u
#define C_FG 0xFFFFu
#define C_ACC 0xFD20u

void fm1_timer5_irq(void)
{
    static uint32_t sub, last, acc;
    uint32_t t0 = fm1_ticks();
    fm1_timer5_ack();
    fm1_input_tick();
    acc += t0 - last;                           /* milliseconds from the 24 MHz TIMER4 */
    last = t0;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        fm1_ms++;
    }
    if (sub++ % 5u == 0u)
        usb_poll();                             /* 2 kHz: all USB SIE traffic lives here */
    if (sub == 10u)
        sub = 0;
}
extern void isr_timer5(void);

static void hexs(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

static void fm1_fault(const fm1_crash_t *c)
{
    char b[12];
    uint32_t t0;
    fm1_audio_stop();
    lcd_wake_now();
    lcd_fill(0, 0, 240, 240, 0xF800u);
    text5x7(8, 8, "CRASH", 3, C_FG, 0xF800u);
    hexs(b, c->vec);
    text5x7(10, 50, b, 2, C_FG, 0xF800u);
    hexs(b, c->pc);
    text5x7(10, 70, b, 2, C_FG, 0xF800u);
    hexs(b, c->emu);
    text5x7(10, 90, b, 2, C_FG, 0xF800u);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 4000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}

static uint32_t cur_preset;
static int load_err;

static void status_draw(void)                   /* the patch name and the engine's CPU load, a few times a second */
{
    char line[24], num[12];
    char *p = line;
    uint32_t last = eng_us_last, peak = eng_us_max;
    const char *s;
    for (s = "CPU "; *s;)
        *p++ = *s++;
    fmt_int(num, (int32_t)(last * 100u / 2902u));
    for (s = num; *s;)
        *p++ = *s++;
    for (s = "% PK "; *s;)
        *p++ = *s++;
    fmt_int(num, (int32_t)(peak * 100u / 2902u));
    for (s = num; *s;)
        *p++ = *s++;
    *p++ = '%';
    while (p < line + 16)
        *p++ = ' ';
    *p = 0;
    text5x7(12, 150, line, 2, C_FG, C_BG);
}

static void screen_idle(void)
{
    char b[16], *p = b;
    lcd_fill(0, 0, 240, 240, C_BG);
    text5x7(12, 24, "MODULAR-1", 4, C_FG, C_BG);
    text5x7(12, 66, FELUCCA_ID, 2, C_ACC, C_BG);
    text5x7(12, 90, FELUCCA_VERSION, 2, C_ACC, C_BG);
    if (load_err) {
        const char *s = "LOAD ERR ";
        while (*s)
            *p++ = *s++;
        fmt_int(p, load_err);
        text5x7(12, 120, b, 2, 0xF800u, C_BG);
    } else {
        text5x7(12, 120, PRESET_LIST[cur_preset].name, 2, C_FG, C_BG);
    }
    text5x7(12, 190, "PROGRAM CHANGE: PATCH", 1, C_FG, C_BG);
    status_draw();
}

static void preset_load(uint32_t n)
{
    int rc;
    if (n >= N_PRESETS)
        return;
    eng_run = 0;
    eng_all_notes_off();
    rc = eng_load(PRESET_LIST[n].data, PRESET_LIST[n].len);
    load_err = -rc;
    cur_preset = n;
    eng_us_max = 0;
    eng_run = rc == 0;
    screen_idle();
}

static void midi_drain(void)                    /* USB-MIDI events -> the engine */
{
    while (mi_r != mi_w) {
        uint32_t pkt = midi_in_q[mi_r % MQ], st = (pkt >> 8) & 0xF0u, n = (pkt >> 16) & 0x7Fu, v = pkt >> 24;
        RING_PUBLISH();
        mi_r++;
        if (st == 0x90u && v) {
            eng_note_on(n, (int32_t)(v * 65536u / 127u));
        } else if (st == 0x80u || st == 0x90u) {
            eng_note_off(n);
        } else if (st == 0xB0u && n == 123u) {
            eng_all_notes_off();
        } else if (st == 0xC0u) {
            preset_load((pkt >> 16) & 0x7Fu);   /* program change: the factory patches */
        }
    }
    midi_in_overflow = 0;
}

static void fm1_main(void)
{
    uint32_t t_btn = 0;
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;     /* the expected 1 MiB part, else no OTA staging */
    if (flash_ok)
        ota_boot_cleanup();                             /* staging area left by an update */
    lcd_init();
    fm1_input_init();
    audio_init();
    usb_start();
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    fm1_delay_ms(30);
    preset_load(0);
    for (;;) {
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (fm1_ms > 30000u && bootguard.pending) {     /* a crash or hang in the first 30 s counts */
            bootguard.pending = 0;
            bootguard.failed = 0;
        }
        midi_drain();
        {
            static uint32_t t_stat;
            if (fm1_ms - t_stat >= 250u) {
                t_stat = fm1_ms;
                status_draw();
            }
        }
        {   /* OCT- + OCT+ (matrix buttons 0 and 1) held 5 s: UBOOT, as in Felucca */
            uint32_t both = 3u;
            if ((fm1_in.buttons & both) != both)
                t_btn = fm1_ms;
            else if (fm1_ms - t_btn > 5000u) {
                fm1_audio_stop();
                lcd_fill(0, 0, 240, 240, C_BG);
                text5x7(12, 110, "UBOOT", 4, C_ACC, C_BG);
                usb_detach();
                fm1_delay_ms(30);
                bootguard.pending = 0;
                fm1_enter_uboot();
            }
        }
        ota_service();                                  /* M-UPGRADE handshake */
        if (usb.ota_req) {                              /* M-UPGRADE upgrade command */
            usb.ota_req = 0;
            eng_run = 0;
            if (flash_ok)
                ota_session();                          /* returns only if nothing was committed */
            preset_load(cur_preset);
        }
        if (usb.uboot_req) {                            /* SysEx F0 22 24 35 7D F7 from the host */
            fm1_audio_stop();
            lcd_fill(0, 0, 240, 240, C_BG);
            text5x7(12, 110, "UBOOT (USB)", 3, C_ACC, C_BG);
            fm1_delay_ms(20);
            usb_detach();
            fm1_delay_ms(30);
            bootguard.pending = 0;
            fm1_enter_uboot();
        }
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d, p3, src, wdt;
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    src = fm1_boot.rst_src;
    wdt = fm1_boot.wdt_con;
    fm1_wdt_arm(0x0D);
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = 0;
        bootguard.pending = 0;
    }
    if (bootguard.pending)
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 2u) {
        bootguard.failed = 0;
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;                                /* flash driver code that must run from RAM */
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    fm1_boot.rst_src = src;
    fm1_boot.wdt_con = (uint8_t)wdt;
    fm1_main();
    for (;;)
        ;
}
