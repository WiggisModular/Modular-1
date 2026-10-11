/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Modified for Modular-1 (morgulbrut). */
/* Boot and main loop, reduced from Felucca's: WDT, boot-loop guard, fatal vectors, then LCD, input (TIMER5
 * IRQ, 10 kHz), audio (ALNK0 IRQ), USB. Kept so the device can always be re-flashed: the boot-loop guard
 * (two failed boots -> UBOOT), OCT- + OCT+ held 5 s -> UBOOT, the OTA command and the UBOOT SysEx. */
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];


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
    if (sub++ % 5u == 0u) {
        usb_poll();                             /* 2 kHz: all USB SIE traffic lives here */
        uart_midi_poll();                       /* the TRS jack's DMA ring (128 bytes = 41 ms) */
    }
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

static void leds_octave(void);
static void page_set(uint32_t p);

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
    sel = 0;
    patch_label = "FROM EDITOR";
    screen_idle();
    return rc;
}
static int m1_store(const uint8_t *patch, uint32_t len)
{
    int rc;
    if (!flash_ok)
        return -1;
    ps_state[0] = (uint32_t)(octave + 8);        /* the state saved with the patch: octave, selected module, preset */
    ps_state[1] = sel;
    ps_state[2] = cur_preset;
    eng_run = 0;                                /* a sector erase takes the CPU for a while: no audio meanwhile */
    rc = ps_save(patch, len);
    eng_run = load_err == 0;
    return rc;
}

/* The preset list the PRESET knob and MIDI program change step through: the preset bank if the editor uploaded one
 * (the occupied flash slots in slot order), else the factory presets compiled into the firmware. */
static uint32_t popcount32(uint32_t v)
{
    uint32_t n = 0;
    for (; v; v &= v - 1u)
        n++;
    return n;
}
static uint32_t list_count(void) { return bank_mask ? popcount32(bank_mask) : N_PRESETS; }
static uint32_t list_slot(uint32_t idx)             /* slot number of the idx-th occupied slot */
{
    uint32_t s;
    for (s = 0; s < M1_SLOTS; s++)
        if ((bank_mask >> s & 1u) && idx-- == 0u)
            return s;
    return 0;
}

static char label_buf[40];

static void preset_load(uint32_t n)
{
    static uint8_t bank_buf[M1_PATCH_MAX];
    uint32_t count = list_count(), len = 0;
    const uint8_t *data;
    const char *name;
    char slot_name[M1_NAME + 1], *p = label_buf;
    if (n >= count)
        return;
    if (bank_mask) {
        if (sl_read(list_slot(n), bank_buf, &len, slot_name))
            return;
        data = bank_buf;
        name = slot_name;
    } else {
        data = PRESET_LIST[n].data;
        len = PRESET_LIST[n].len;
        name = PRESET_LIST[n].name;
    }
    if (m1_apply(data, len) == 0)
        m1_set_current(data, len);
    cur_preset = n;
    p = put(p, name);
    *p = 0;
    patch_label = label_buf;
    screen_idle();
}

static void preset_step(int32_t d)                  /* the PRESET knob */
{
    int32_t n = (int32_t)list_count(), i = ((int32_t)cur_preset + d) % n;
    preset_load((uint32_t)(i < 0 ? i + n : i));
}

/* proto.c hooks: the preset bank */
static int m1_slot_write(uint32_t slot, const char *name, const uint8_t *p, uint32_t len)
{
    uint8_t was = eng_run;
    int rc;
    if (!flash_ok)
        return -1;
    eng_run = 0;                                /* the erase and program windows have the IRQs off */
    rc = sl_write(slot, name, p, len);
    eng_run = was;
    if (cur_preset >= list_count())
        cur_preset = 0;
    return rc;
}
static int m1_slot_erase(uint32_t slot)
{
    uint8_t was = eng_run;
    int rc;
    if (!flash_ok)
        return -1;
    eng_run = 0;
    rc = sl_erase(slot);
    eng_run = was;
    if (cur_preset >= list_count())
        cur_preset = 0;
    return rc;
}
static int m1_slot_info(uint32_t slot, char *name, uint32_t *len)
{
    sl_hdr_t h;
    uint32_t i;
    if (!flash_ok || !(bank_mask >> slot & 1u) || sl_head(slot, &h, 0))
        return 0;
    *len = h.len;
    for (i = 0; i < M1_NAME; i++)
        name[i] = h.name[i];
    return 1;
}

static void patch_boot(void)                    /* the stored patch if there is one, else the first factory patch */
{
    static uint8_t buf[M1_PATCH_MAX];
    uint32_t len, seq, sector;
    if (flash_ok)
        sl_scan();
    if (flash_ok && ps_load(buf, &len, &seq, &sector) == 0 && m1_apply(buf, len) == 0) {
        m1_set_current(buf, len);
        octave = ps_loaded[0] ? (int32_t)ps_loaded[0] - 8 : 0;   /* the state that was saved with it (0: a record from before) */
        octave = octave < -3 ? -3 : octave > 3 ? 3 : octave;
        sel = ps_loaded[1] < (uint32_t)eng_nmodules() ? ps_loaded[1] : 0u;
        cur_preset = ps_loaded[2] < list_count() ? ps_loaded[2] : 0u;
        patch_label = "STORED PATCH";
        leds_octave();
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
            preset_load((pkt >> 16) & 0x7Fu);   /* program change: a preset of the list */
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

/* SELECT (encoder 0) steps through the modules of the patch. The four knobs (encoders 2..5, Felucca's panel table) change
 * the parameters the selected module's matrix assigns to them (on the FM-1 panel: k1..k4): a click is 1/100 of the range
 * (the parameter's own step if that is larger, so a choice moves one entry), five times that when turned fast. The new
 * values go into the stored copy of the patch too. */
static void knobs_poll(void)
{
    static uint32_t last_ms[4];
    int32_t st[7];
    uint32_t k, j, n = (uint32_t)eng_nmodules();
    for (k = 0; k < 7u; k++)
        st[k] = fm1_enc_take(k);
    if (st[6]) {
        preset_step(st[6]);
        return;
    }
    if (page && st[0])                              /* SELECT: back to the module page */
        page_set(page);
    if (page) {                                     /* K1..K4 of an arpeggiator page */
        for (k = 0; k < 4u; k++) {
            uint32_t fast = fm1_ms - last_ms[k] < 80u;
            if (st[2u + k]) {
                last_ms[k] = fm1_ms;
                if (arp_knob(page, k, st[2u + k], (int)fast) && !ui_dirty)
                    ui_dirty = 1;
            }
        }
        if (!st[0])
            return;
    }
    if (!n)
        return;
    if (sel >= n)
        sel = 0;
    if (st[0]) {
        int32_t m = ((int32_t)sel + st[0]) % (int32_t)n;
        sel = (uint32_t)(m < 0 ? m + (int32_t)n : m);
        ui_dirty = 2;
    }
    for (k = 0; k < 4u; k++) {
        uint32_t mask;
        int panel_knob = -1;
        if (!st[2u + k])
            continue;
        mask = eng_knob_mask(sel, k);
        for (j = 0; j < DEFS[eng_type(sel)].npar; j++) {
            const def_t *d = &DEFS[eng_type(sel)];
            int32_t inc, v, pn;
            uint32_t fast = fm1_ms - last_ms[k] < 80u;
            if (!(mask >> j & 1u))
                continue;
            if ((pn = eng_knob_panel(sel, k, j)) >= 0) {   /* the panel's knob drives it: turn that one (as with the panel selected) */
                panel_knob = pn;
                continue;
            }
            inc = d->pinc[j];
            if (eng_param_is_log(sel, j)) {              /* the cutoff: 1/100 of its octaves per click */
                v = eng_step_param(sel, j, st[2u + k], fast ? 5 * 655 : 655);
            } else {
                if (inc < ENG_ONE && fast)
                    inc *= 5;
                v = eng_get_param(sel, j) + st[2u + k] * inc;
                v = v < d->pmin[j] ? d->pmin[j] : v > d->pmax[j] ? d->pmax[j] : v;
            }
            eng_set_param(sel, j, v);
            m1_patch_param(sel, j, v);
        }
        if (panel_knob >= 0) {
            const def_t *fd = &DEFS[T_FM1];
            int32_t inc = fd->pinc[k], v;
            if (inc < ENG_ONE && fm1_ms - last_ms[k] < 80u)
                inc *= 5;
            v = eng_get_param((uint32_t)panel_knob, k) + st[2u + k] * inc;
            v = v < fd->pmin[k] ? fd->pmin[k] : v > fd->pmax[k] ? fd->pmax[k] : v;
            eng_set_param((uint32_t)panel_knob, k, v);
            m1_patch_param((uint32_t)panel_knob, k, v);
        }
        if (mask) {
            last_ms[k] = fm1_ms;
            if (!ui_dirty)
                ui_dirty = 1;
        }
    }
}

static uint8_t key_note[27];                     /* the note each held key started, so a release (or an octave change in between) ends the right one */
static void keys_poll(void)                     /* the FM-1's 27 keys (bit 0 = F3 = MIDI 53), shifted by the octave, -> the engine */
{
    static uint32_t prev;
    uint32_t now = fm1_in.notes, diff = now ^ prev, n;
    prev = now;
    for (n = 0; diff && n < 27u; n++) {
        if (!(diff & (1u << n)))
            continue;
        diff &= ~(1u << n);
        if (now & (1u << n)) {
            int32_t note = 53 + (int32_t)n + 12 * octave;
            key_note[n] = (uint8_t)(note < 0 ? 0 : note > 127 ? 127 : note);
            eng_note_on(key_note[n], ENG_ONE);
        } else {
            eng_note_off(key_note[n]);
        }
    }
}

/* PLAY: red while the arpeggiator is stopped, green while it runs. The green LED sits at matrix column 8, row bit 1,
 * where there is no key (found on the hardware by Felucca); the red one is PLAY's own. ARP / SEQ / LFO light up
 * while their page shows. */
#define BTN_ARP 10u
#define BTN_SEQ 11u
#define BTN_PLAY 12u
#define BTN_LFO 5u                  /* the request said "BTN 52": there is no id 52 (buttons are 0..13), 5 is LFO in Felucca's table */
static void led_breath_key(uint32_t id, int on)     /* a button's LED breathes (the HAL's one shared speed, fm1_br_step) */
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id) {
                if (on)
                    fm1_led_breath[p] |= (uint8_t)(1u << r);
                else
                    fm1_led_breath[p] &= (uint8_t)~(1u << r);
            }
}

static void leds_arp(void)
{
    int run = arp_play;
    fm1_led_key(BTN_PLAY, !run);                    /* stopped: PLAY's red LED; running: the green one breathes */
    fm1_led[8] &= (uint8_t)~2u;
    if (run)
        fm1_led_breath[8] |= 2u;
    else
        fm1_led_breath[8] &= (uint8_t)~2u;
    fm1_led_key(BTN_ARP, page == 1u);
    fm1_led_key(BTN_SEQ, page == 2u);
    fm1_led_key(BTN_LFO, page == 3u);
}

static void page_set(uint32_t p)    /* the same button again goes back to the module page */
{
    page = (uint8_t)(page == p ? 0u : p);
    leds_arp();
    lcd_fill(0, 24, 240, 216, C_BG);
    view_draw(1);
}

static void arp_toggle(void)
{
    arp_play = !arp_play;
    leds_arp();
    if (page)
        ui_dirty = ui_dirty ? ui_dirty : 1;
}

/* OCT- / OCT+ light up while the octave is shifted */
static void leds_octave(void)
{
    uint32_t m = (uint32_t)(octave < 0 ? -octave : octave);
    led_breath_key(0, octave < 0);                  /* the further from 0 the faster: 1 slow, 2 medium, 3 fast */
    led_breath_key(1, octave > 0);
    fm1_br_step = m >= 3u ? 4u : m == 2u ? 2u : 1u;   /* (the PLAY breath runs at the same speed) */
}

/* SAVE: the current state (the patch with its edited parameters, the octave, the selected module and preset) to flash */
static void save_state(void)
{
    if (!flash_ok || !m1_cur_len) {
        ui_msg("NO FLASH", 1500);
        return;
    }
    ui_msg("SAVING", 5000);
    ui_msg(m1_store(m1_cur, m1_cur_len) ? "ERROR" : "SAVED", 1500);
}

/* The panel buttons (matrix ids as in Felucca's panel table; unknown ones are shown so they can be assigned):
 * OCT- 0, OCT+ 1: octave; SAVE 9: save the state. */
#define BTN_OCT_DOWN 0u
#define BTN_OCT_UP 1u
#define BTN_SAVE 9u
/* the panel's buttons (ids 0..13); the unused ones do nothing yet and show no banner */
#define BTN_FX 2u
#define BTN_SEL 3u
#define BTN_ENV 4u
#define BTN_EDIT 6u
#define BTN_GLO 7u
#define BTN_REC 13u
#define BTN_HOME 8u                     /* HOME: back to the module page, on the FM-1 panel module of the patch */

static void led_glow(uint32_t id)                   /* the HAL's dim glow (~1/30 of lit) for a button's LED */
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id)
                fm1_led_dim[p] |= (uint8_t)(1u << r);
}

static void leds_avail(void)                        /* the page buttons glow dimly (lit while their page shows) */
{
    led_glow(BTN_HOME);
    led_glow(BTN_ARP);
    led_glow(BTN_SEQ);
    led_glow(BTN_LFO);
}

static void home_press(void)
{
    uint32_t n = (uint32_t)eng_nmodules(), i;
    for (i = 0; i < n; i++)
        if (eng_type(i) == T_FM1) {
            sel = i;
            break;
        }
    if (page) {
        page = 0;
        leds_arp();
        lcd_fill(0, 24, 240, 216, C_BG);
    }
    view_draw(1);
}
static void buttons_poll(void)
{
    uint32_t rel, p = fm1_input_edges(&rel);
    if (p & (1u << BTN_OCT_DOWN))
        octave = octave > -3 ? octave - 1 : octave;
    if (p & (1u << BTN_OCT_UP))
        octave = octave < 3 ? octave + 1 : octave;
    if (p & ((1u << BTN_OCT_DOWN) | (1u << BTN_OCT_UP))) {
        leds_octave();
        {
            char m[12], num[8], *q = m;
            q = put(q, "OCTAVE ");
            if (octave > 0)
                *q++ = '+';
            fmt_int(num, octave);
            put(q, num);
            ui_msg(m, 1000);
        }
    }
    if (p & (1u << BTN_SAVE))
        save_state();
    if (p & (1u << BTN_HOME))
        home_press();
    if (p & (1u << BTN_ARP))
        page_set(1);
    if (p & (1u << BTN_SEQ))
        page_set(2);
    if (p & (1u << BTN_LFO))
        page_set(3);
    if (p & (1u << BTN_PLAY))
        arp_toggle();
}

#ifdef M1_SPLASH                                        /* build with -DM1_SPLASH to bring the splash screen back */
#define SPLASH_MS 10000u
#else
#define SPLASH_MS 0u                                    /* switched off for now */
#endif

static void splash_draw(void)
{
    lcd_fill(0, 0, 240, 240, C_BG);
    lcd_fill(12, 84, 216, 3, C_ACC);
    text5x7(12, 96, "MODULAR-1", 4, C_FG, C_BG);
    lcd_fill(12, 132, 216, 3, C_ACC);
    text5x7(12, 146, "PATCHABLE SYNTH FOR FM-1", 1, C_DIM, C_BG);
    text5x7(12, 160, FELUCCA_VERSION, 1, C_ACC, C_BG);
}

static void fm1_main(void)
{
    uint32_t t_btn = 0;
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;     /* the expected 1 MiB part, else no OTA staging */
    if (flash_ok)
        ota_boot_cleanup();                             /* staging area left by an update */
    lcd_init();
    if (SPLASH_MS)
        splash_draw();
    fm1_input_init();
    fm1_adc_init();
    master_boot();
    audio_init();
    usb_start();
    uart_midi_init();                                   /* before TIMER5: PORTH read-modify-write */
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    fm1_delay_ms(30);
    patch_boot();
    while (fm1_ms < SPLASH_MS) {                        /* keep the splash up, USB stays served */
        fm1_wdt_feed();
        usb_retry(fm1_ms);
    }
    lcd_fill(0, 0, 240, 240, C_BG);
    ui_dirty = 2;
    leds_arp();
    leds_avail();
    for (;;) {
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (fm1_ms > 30000u && bootguard.pending) {     /* a crash or hang in the first 30 s counts */
            bootguard.pending = 0;
            bootguard.failed = 0;
        }
        midi_drain();
        keys_poll();
        buttons_poll();
        knobs_poll();
        master_poll();
        {
            if (msg_until && fm1_ms >= msg_until)
                ui_msg_clear();
            if (ui_dirty) {
                int full = ui_dirty > 1;
                ui_dirty = 0;
                view_draw(full);
            }
            au_step_poll(page);
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
