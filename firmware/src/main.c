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

static int32_t enc_acc[7];                       /* detent steps per encoder since boot (shown on the LCD: diagnostics) */
static uint32_t cur_preset;
static int load_err;
static const char *patch_label = "";

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
    {   /* the panel knobs of an FM-1 panel module */
        char kn[32], *q = kn;
        int fm = eng_find(T_FM1);
        uint32_t k;
        if (fm < 0) {
            for (s = "NO FM-1 PANEL MODULE: KNOBS OFF"; *s;)
                *q++ = *s++;
        } else {
            for (k = 0; k < 4u; k++) {
                *q++ = 'K';
                *q++ = (char)('1' + k);
                *q++ = ' ';
                fmt_int(num, (eng_get_param((uint32_t)fm, FM1_P_K1 + k) + 328) / 655);
                for (s = num; *s;)
                    *q++ = *s++;
                *q++ = ' ';
                if (k < 3u)
                    *q++ = ' ';
            }
        }
        while (q < kn + 31)
            *q++ = ' ';
        *q = 0;
        text5x7(12, 176, kn, 1, C_FG, C_BG);
    }
    {   /* every encoder: detent steps / raw transitions since boot (a dead knob shows 0/0) */
        char ln[32], *q;
        uint32_t e, row;
        for (row = 0; row < 3u; row++) {
            q = ln;
            for (e = row * 3u; e < row * 3u + 3u && e < 7u; e++) {
                *q++ = (char)('0' + e);
                *q++ = ':';
                fmt_int(num, enc_acc[e]);
                for (s = num; *s;)
                    *q++ = *s++;
                *q++ = '/';
                fmt_int(num, (int32_t)fm1_in_stat.enc_moves[e]);
                for (s = num; *s;)
                    *q++ = *s++;
                *q++ = ' ';
                *q++ = ' ';
            }
            while (q < ln + 28)
                *q++ = ' ';
            *q = 0;
            text5x7(12, 200u + 9u * row, ln, 1, 0x07E0u, C_BG);
        }
    }
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
        text5x7(12, 120, patch_label, 2, C_FG, C_BG);
    }
    status_draw();
}

/* proto.c hooks */
static int m1_flash_ok(void) { return flash_ok; }
static void m1_send(const uint8_t *f, uint32_t n) { (void)ota_wire_send(f, n); }
static int m1_apply(const uint8_t *patch, uint32_t len)
{
    int rc;
    eng_run = 0;                                /* the ISR renders silence while the patch is replaced */
    rc = eng_load(patch, len);
    load_err = -rc;
    eng_us_max = 0;
    eng_run = rc == 0;
    patch_label = "FROM EDITOR";
    screen_idle();
    return rc;
}
static int m1_store(const uint8_t *patch, uint32_t len)
{
    if (!flash_ok)
        return -1;
    eng_run = 0;                                /* a sector erase takes the CPU for a while: no audio meanwhile */
    {
        int rc = ps_save(patch, len);
        eng_run = load_err == 0;
        return rc;
    }
}

static void preset_load(uint32_t n)
{
    if (n >= N_PRESETS)
        return;
    if (m1_apply(PRESET_LIST[n].data, PRESET_LIST[n].len) == 0)
        m1_set_current(PRESET_LIST[n].data, PRESET_LIST[n].len);
    cur_preset = n;
    patch_label = PRESET_LIST[n].name;
    screen_idle();
}

static void patch_boot(void)                    /* the stored patch if there is one, else the first factory patch */
{
    static uint8_t buf[M1_PATCH_MAX];
    uint32_t len, seq, sector;
    if (flash_ok && ps_load(buf, &len, &seq, &sector) == 0 && m1_apply(buf, len) == 0) {
        m1_set_current(buf, len);
        patch_label = "STORED PATCH";
        screen_idle();
        return;
    }
    preset_load(0);
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

/* MASTER pot (ADC 0..1023, square law): read before the audio starts, then smoothed per main-loop pass */
static int32_t master_knob = 512 * 16;
static void master_poll(void)
{
    int32_t a = fm1_adc_read(FM1_ADC_MASTER);
    if (a >= 0) {
        uint32_t k;
        master_knob += (a * 16 - master_knob) / 8;
        k = (uint32_t)(master_knob / 16);
        master_q12 = (k * k) >> 8;
    }
}

static void master_boot(void)
{
    uint32_t i;
    (void)fm1_adc_read(FM1_ADC_MASTER);                 /* the first conversion after the pin setup: dropped */
    for (i = 0; i < 3u; i++) {
        int32_t a = fm1_adc_read(FM1_ADC_MASTER);
        if (a >= 0) {
            master_knob = a * 16;
            master_q12 = ((uint32_t)a * (uint32_t)a) >> 8;
            return;
        }
    }
}

/* The four knobs (encoders 2..5 in Felucca's panel table: K1..K4) move the k1..k4 parameters of the patch's FM-1
 * panel module: a click is 0.01 of the range, 0.05 when turned fast. The values are written into the stored copy
 * too (STORE keeps them). */
static void knobs_poll(void)
{
    static uint32_t last_ms[4];
    int32_t st[7];
    int fm = eng_find(T_FM1);
    uint32_t k;
    for (k = 0; k < 7u; k++) {
        st[k] = fm1_enc_take(k);
        enc_acc[k] += st[k];
    }
    for (k = 0; k < 4u; k++) {
        int32_t s = st[2u + k], v;
        if (!s || fm < 0)
            continue;
        v = eng_get_param((uint32_t)fm, FM1_P_K1 + k) + s * (fm1_ms - last_ms[k] < 80u ? 3277 : 655);
        v = v < 0 ? 0 : v > 65536 ? 65536 : v;
        last_ms[k] = fm1_ms;
        eng_set_param((uint32_t)fm, FM1_P_K1 + k, v);
        m1_patch_param((uint32_t)fm, FM1_P_K1 + k, v);
    }
}

static void keys_poll(void)                     /* the FM-1's 27 keys (bit 0 = F3 = MIDI 53) -> the engine */
{
    static uint32_t prev;
    uint32_t now = fm1_in.notes, diff = now ^ prev, n;
    prev = now;
    for (n = 0; diff && n < 27u; n++) {
        if (!(diff & (1u << n)))
            continue;
        diff &= ~(1u << n);
        if (now & (1u << n))
            eng_note_on(53u + n, ENG_ONE);
        else
            eng_note_off(53u + n);
    }
}

static void fm1_main(void)
{
    uint32_t t_btn = 0;
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;     /* the expected 1 MiB part, else no OTA staging */
    if (flash_ok)
        ota_boot_cleanup();                             /* staging area left by an update */
    lcd_init();
    fm1_input_init();
    fm1_adc_init();
    master_boot();
    audio_init();
    usb_start();
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    fm1_delay_ms(30);
    patch_boot();
    for (;;) {
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (fm1_ms > 30000u && bootguard.pending) {     /* a crash or hang in the first 30 s counts */
            bootguard.pending = 0;
            bootguard.failed = 0;
        }
        midi_drain();
        keys_poll();
        knobs_poll();
        master_poll();
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
        {   /* the editor's SysEx (proto.c) before the update protocol's */
            const uint8_t *fp;
            uint32_t fn;
            if (ota_frame_get(&fp, &fn) && m1_handle(fp, fn))
                ota_frame_done();
        }
        ota_service();                                  /* M-UPGRADE handshake */
        if (usb.ota_req) {                              /* M-UPGRADE upgrade command */
            usb.ota_req = 0;
            eng_run = 0;
            if (flash_ok)
                ota_session();                          /* returns only if nothing was committed */
            if (m1_apply(m1_cur, m1_cur_len) == 0)
                m1_set_current(m1_cur, m1_cur_len);
            else
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
