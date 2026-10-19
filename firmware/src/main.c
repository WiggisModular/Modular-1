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
static void leds_arp(void);
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
    eng_run = 1;                                /* a voice that did not load renders silence by itself */
    sel = 0;
    patch_label = "FROM EDITOR";
    screen_idle();
    return rc;
}
/* The menu's settings ride in the upper bits of the first state word (the low 4: the octave + 8):
 *   7 mixer is in the blob's tail  9 cpu_bar  10 accel  11 clock_in  12 prog_chg  15 set (older records have 0 here)
 * (records before the four voices also have 8 leds, 13..14 midi_to, 16..29 the levels of voices 1 / 2, 30..31 their mutes) */
static uint32_t state_pack(void)
{
    return 1u << 7 | (uint32_t)cfg.cpu_bar << 9 | (uint32_t)cfg.accel << 10 | (uint32_t)cfg.clock_in << 11 |
           (uint32_t)cfg.prog_chg << 12 | 1u << 15;
}

static void state_unpack(uint32_t w)
{
    if (!(w >> 15 & 1u))
        return;                                         /* a record from before: the defaults */
    cfg.cpu_bar = w >> 9 & 1u;
    cfg.accel = w >> 10 & 1u;
    cfg.clock_in = w >> 11 & 1u;
    cfg.prog_chg = w >> 12 & 1u;
    if (!(w >> 7 & 1u)) {                               /* the mixer of a record from before (voices 1 and 2) */
        mix_vol[0] = (uint8_t)(w >> 16 & 127u) > 100u ? 100u : (uint8_t)(w >> 16 & 127u);
        mix_vol[1] = (uint8_t)(w >> 23 & 127u) > 100u ? 100u : (uint8_t)(w >> 23 & 127u);
        mix_mute[0] = w >> 30 & 1u;
        mix_mute[1] = w >> 31 & 1u;
    }
    mix_apply_all();
}

/* The saved state (SAVE, the editor's STORE): one blob in the patch store (patchstore.c), the whole state of the panel:
 *   0 0xB2  1 version 4  2..5 the length of each voice's patch (u16)  6.. the arpeggiators' settings (ARP_PACK bytes each)
 *   then the patches as they run now (parameter edits included), then the tail: the MIDI channels (2), the LED mode, the
 *   mixer levels (2), the mute bits, then the drum machine (dr_pack).
 * With the header's words: octave and settings (state_pack), the selected module of each voice (byte per voice in word 1) and
 * the preset of each (word 2). Version 3 (the same without the drums) and version 2 (four voices, voices 3 and 4 dropped) are still read; so is version 1 (two
 * voices, before the four-voice firmware): its lengths are 2..5, its arpeggiators follow at 6, its tail is the two
 * channels and the LED mode. An even older record holds just voice 1's
 * patch (starts with 0xA1). */
#define BLOB_MAGIC 0xB2u
#define ARP_PACK 13u
static uint8_t blob[PS_MAX] ENG_POOL;

static void arp_pack(uint8_t *d, const arp_cfg_t *c)
{
    d[0] = c->mode; d[1] = c->scale; d[2] = c->root; d[3] = c->octaves; d[4] = c->chaos; d[5] = c->vchaos; d[6] = c->rchaos;
    d[7] = c->steps; d[8] = c->pulses; d[9] = c->rot; d[10] = c->rate; d[11] = (uint8_t)c->bpm; d[12] = (uint8_t)(c->bpm >> 8);
}

static void arp_unpack(uint32_t v, const uint8_t *d)
{
    arp_t *keep = arp_p;
    arp_p = &AV[v];
    arp_c.mode = d[0]; arp_c.scale = d[1]; arp_c.root = d[2]; arp_c.octaves = d[3]; arp_c.chaos = d[4]; arp_c.vchaos = d[5];
    arp_c.rchaos = d[6]; arp_c.steps = d[7]; arp_c.pulses = d[8]; arp_c.rot = d[9]; arp_c.rate = d[10];
    arp_c.bpm = (uint16_t)(d[11] | d[12] << 8);
    arp_fix();                                  /* anything out of range is clamped */
    arp_dirty = 1;
    arp_p = keep;
}

/* The drum machine (version 4 of the blob, after the tail): the patterns, levels, decays, length, pattern number, tempo, MIDI
 * channel, mixer level and mute. */
#define DRUM_PACK (2u * DR_N + 2u * DR_N + 1u + 1u + 2u + 1u + 2u)
static void dr_pack(uint8_t *d)
{
    uint32_t p, n = 0;
    for (p = 0; p < DR_N; p++) {
        d[n++] = (uint8_t)dr.pat[p];
        d[n++] = (uint8_t)(dr.pat[p] >> 8);
    }
    for (p = 0; p < DR_N; p++)
        d[n++] = dr.level[p];
    for (p = 0; p < DR_N; p++)
        d[n++] = dr.decay[p];
    d[n++] = dr.steps;
    d[n++] = dr.preset;
    d[n++] = (uint8_t)dr.bpm;
    d[n++] = (uint8_t)(dr.bpm >> 8);
    d[n++] = cfg.ch[DRUM_V];
    d[n++] = mix_vol[DRUM_V];
    d[n++] = mix_mute[DRUM_V];
}

static void dr_unpack(const uint8_t *d)
{
    uint32_t p, n = 0;
    for (p = 0; p < DR_N; p++, n += 2u)
        dr.pat[p] = (uint16_t)(d[n] | d[n + 1u] << 8);
    for (p = 0; p < DR_N; p++)
        dr.level[p] = d[n++];
    for (p = 0; p < DR_N; p++)
        dr.decay[p] = d[n++];
    dr.steps = d[n++];
    dr.preset = d[n++];
    dr.bpm = (uint16_t)(d[n] | d[n + 1u] << 8);
    n += 2u;
    cfg.ch[DRUM_V] = d[n] > 16u ? 10u : d[n];
    n++;
    mix_vol[DRUM_V] = d[n] > 100u ? 100u : d[n];
    n++;
    mix_mute[DRUM_V] = d[n] & 1u;
    dr_fix();
}

#define BLOB_HDR (2u + 2u * ENG_VOICES)
#define BLOB_TAIL (ENG_VOICES + 1u + ENG_VOICES + 1u)
static uint32_t blob_build(void)                /* 0 if it does not fit */
{
    uint32_t n = BLOB_HDR, v, i, total = BLOB_HDR + ENG_VOICES * ARP_PACK + BLOB_TAIL + DRUM_PACK;
    uint8_t mutes = 0;
    for (v = 0; v < ENG_VOICES; v++)
        total += m1_cur_lenv[v];
    if (total > PS_MAX)
        return 0;
    blob[0] = BLOB_MAGIC;
    blob[1] = 4;
    for (v = 0; v < ENG_VOICES; v++) {
        blob[2u + 2u * v] = (uint8_t)m1_cur_lenv[v];
        blob[3u + 2u * v] = (uint8_t)(m1_cur_lenv[v] >> 8);
    }
    for (v = 0; v < ENG_VOICES; v++, n += ARP_PACK)
        arp_pack(blob + n, &AV[v].cfg);
    for (v = 0; v < ENG_VOICES; v++)
        for (i = 0; i < m1_cur_lenv[v]; i++)
            blob[n++] = m1_curv[v][i];
    for (v = 0; v < ENG_VOICES; v++)
        blob[n++] = cfg.ch[v];
    blob[n++] = cfg.leds;
    for (v = 0; v < ENG_VOICES; v++) {
        blob[n++] = mix_vol[v];
        mutes |= (uint8_t)(mix_mute[v] << v);
    }
    blob[n++] = mutes;
    dr_pack(blob + n);
    return n + DRUM_PACK;
}

static int m1_store(const uint8_t *patch, uint32_t len)        /* (the arguments are the editor's: the state is stored whole) */
{
    int rc;
    uint32_t n, v;
    (void)patch;
    (void)len;
    if (!flash_ok || !m1_cur_lenv[0])
        return -1;
    n = blob_build();
    if (!n)
        return -2;
    ps_state[0] = (uint32_t)(octave + 8) | state_pack();   /* octave and settings */
    ps_state[1] = ps_state[2] = ps_state[3] = 0;
    for (v = 0; v < ENG_VOICES; v++) {
        ps_state[1] |= (vs[v].module & 255u) << (8u * v);  /* the selected module and the preset of each voice */
        ps_state[2] |= (vs[v].preset & 255u) << (8u * v);
    }
    eng_run = 0;                                /* a sector erase takes the CPU for a while: no audio meanwhile */
    rc = ps_save(blob, n);
    eng_run = 1;
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

static char label_buf[ENG_VOICES][40];           /* each voice's preset name */

static void preset_load(uint32_t n)
{
    static uint8_t bank_buf[M1_PATCH_MAX] ENG_POOL;
    uint32_t count = list_count(), len = 0;
    const uint8_t *data;
    const char *name;
    char slot_name[M1_NAME + 1], *p = label_buf[eng_voice()];
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
    patch_label = label_buf[eng_voice()];
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

/* The loading screen: the name, then what is being loaded and a bar, from the first LCD write until the patches are up */
static void splash_draw(void)
{
    lcd_fill(0, 0, 240, 240, C_BG);
    lcd_fill(12, 60, 216, 3, C_ACC);
    text5x7(12, 72, "MODULAR-1", 4, C_FG, C_BG);
    lcd_fill(12, 108, 216, 3, C_ACC);
    text5x7(12, 122, "PATCHABLE SYNTH FOR FM-1", 1, C_DIM, C_BG);
    text5x7(12, 136, FELUCCA_VERSION, 1, C_ACC, C_BG);
}

#define SPLASH_MS 5000u                         /* the loading screen stays up at least this long */
static const char BOOT_READY[] = "READY";
static uint32_t t_splash;                       /* fm1_ms when it came up */
static const char *boot_what;                   /* what boot_step last printed */

/* what is being loaded now; the bar is the time left of SPLASH_MS (the steps are quicker than that) */
static void boot_step(const char *what)
{
    static uint32_t drawn = ~0u;
    char line[32], *p;
    uint32_t el = fm1_ms - t_splash, w = el >= SPLASH_MS ? 216u : el * 216u / SPLASH_MS;
    if (!booting)
        return;
    if (what != boot_what) {
        boot_what = what;
        p = put(line, what == BOOT_READY ? "" : "LOADING ");
        p = put(p, what);
        pad(p, line, 26);
        text5x7(12, 176, line, 1, C_FG, C_BG);
    }
    if (w != drawn) {
        drawn = w;
        lcd_fill(12, 196, w, 6, C_ACC);
        lcd_fill(12 + w, 196, 216u - w, 6, 0x2104u);
    }
}

static const char *boot_msg;                    /* shown in the bar once the pages are up */
static uint32_t boot_msg_ms;

static const char *const VOICE_LINE[ENG_VOICES] = {"VOICE 1", "VOICE 2"};
static void voice_step(uint32_t v)              /* the loading screen's line for voice v (0..ENG_VOICES-1) */
{
    boot_step(VOICE_LINE[v]);
}

static void voices_boot(uint32_t from)           /* voices from..ENG_VOICES run the factory presets (voice n the n-th) */
{
    uint32_t v;
    for (v = from; v < ENG_VOICES; v++) {
        voice_step(v);
        eng_voice_select(v);
        preset_load(v < list_count() ? v : 0u);
    }
    eng_voice_select(0);
    screen_idle();
}

/* one voice from the saved state: its patch, the module the knobs act on, the preset it came from */
static int voice_restore(uint32_t v, const uint8_t *p, uint32_t len, uint32_t module, uint32_t preset)
{
    int rc;
    voice_step(v);
    eng_voice_select(v);
    rc = m1_apply(p, len);
    if (rc == 0) {
        m1_set_current(p, len);
        sel = module < (uint32_t)eng_nmodules() ? module : 0u;
        cur_preset = preset < list_count() ? preset : 0u;
        patch_label = "STORED PATCH";
    }
    eng_voice_select(0);
    return rc;
}

static int boot_blob(const uint8_t *b, uint32_t len)    /* the saved state (blob_build); 0 = restored */
{
    enum { SV_MAX = 4 };                                /* the most voices a record can hold (version 2) */
    uint32_t nv = b[1] == 2u ? 4u : 2u, hdr = 2u + 2u * nv, at = hdr + nv * ARP_PACK, v, l[SV_MAX] = {0, 0, 0, 0}, total, rv;
    if (len < at || (b[1] < 1u || b[1] > 4u))
        return -1;
    for (v = 0, total = at; v < nv; v++) {
        l[v] = (uint32_t)b[2u + 2u * v] | (uint32_t)b[3u + 2u * v] << 8;
        total += l[v];
    }
    if (!l[0] || total > len)
        return -1;
    rv = nv < ENG_VOICES ? nv : ENG_VOICES;             /* the voices this firmware has */
    {
        const uint8_t *p = b + at;
        for (v = 0; v < rv; p += l[v], v++) {
            uint32_t module, preset;
            if (b[1] == 1u) {                           /* two voices: 16 bit modules, voice 2's preset + 1 in word 3 */
                module = v ? ps_loaded[1] >> 16 : ps_loaded[1] & 0xFFFFu;
                preset = v ? (ps_loaded[3] ? ps_loaded[3] - 1u : 0u) : ps_loaded[2];
            } else {
                module = ps_loaded[1] >> (8u * v) & 255u;
                preset = ps_loaded[2] >> (8u * v) & 255u;
            }
            if (l[v]) {
                if (voice_restore(v, p, l[v], module, preset) && v == 0)
                    return -2;
            } else {
                voices_boot(v);
                break;
            }
        }
    }
    if (rv < ENG_VOICES)
        voices_boot(rv);
    {                                                   /* the tail: channels, LED mode, mixer (nv voices' worth) */
        const uint8_t *t = b + total;
        uint32_t left = len - total;
        if (b[1] == 1u && left >= 3u) {
            cfg.ch[0] = t[0] > 16u ? 1u : t[0];
            cfg.ch[1] = t[1] > 16u ? 2u : t[1];
            cfg.leds = t[2] > 2u ? 2u : t[2];
        } else if (b[1] != 1u && left >= 2u * nv + 2u) {
            for (v = 0; v < ENG_VOICES; v++) {
                cfg.ch[v] = t[v] > 16u ? (uint8_t)(v + 1u) : t[v];
                mix_vol[v] = t[nv + 1u + v] > 100u ? 100u : t[nv + 1u + v];
                mix_mute[v] = t[2u * nv + 1u] >> v & 1u;
            }
            cfg.leds = t[nv] > 2u ? 2u : t[nv];
            if (b[1] == 4u && left >= 2u * nv + 2u + DRUM_PACK)
                dr_unpack(t + 2u * nv + 2u);
        }
    }
    for (v = 0; v < rv; v++)
        arp_unpack(v, b + hdr + v * ARP_PACK);
    arp_sync_scale();
    mix_apply_all();
    return 0;
}

static void patch_boot(void)                    /* the stored state if there is one, else the first factory patches */
{
    static uint8_t buf[PS_MAX] ENG_POOL;
    uint32_t len, seq, sector;
    if (flash_ok)
        sl_scan();
    if (flash_ok && ps_load(buf, &len, &seq, &sector) == 0) {
        int ok = 0;
        if (buf[0] == BLOB_MAGIC)
            ok = boot_blob(buf, len) == 0;
        else if (m1_apply(buf, len) == 0) {     /* a record from before: voice 1's patch alone */
            m1_set_current(buf, len);
            sel = ps_loaded[1] < (uint32_t)eng_nmodules() ? ps_loaded[1] : 0u;
            cur_preset = ps_loaded[2] < list_count() ? ps_loaded[2] : 0u;
            patch_label = "STORED PATCH";
            voices_boot(1u);
            ok = 1;
        }
        if (ok) {
            octave = (ps_loaded[0] & 15u) ? (int32_t)(ps_loaded[0] & 15u) - 8 : 0;   /* the state that was saved with it (0: a record from before) */
            octave = octave < -3 ? -3 : octave > 3 ? 3 : octave;
            state_unpack(ps_loaded[0]);
            leds_avail();
            leds_octave();
            boot_msg = buf[0] == BLOB_MAGIC ? "STATE RESTORED" : "PATCH RESTORED";
            boot_msg_ms = 2000;
            return;
        }
        voices_boot(0u);
        boot_msg = "RESTORE FAILED";                /* a record was there but did not load: the factory presets run */
        boot_msg_ms = 3000;
        return;
    }
    voices_boot(0u);
}

/* MENU > DEFAULTS: the settings, mixer, arpeggiators and the voices as they were made, and the saved state erased */
static void defaults_now(void)
{
    uint32_t v;
    for (v = 0; v < ENG_VOICES; v++)
        eng_all_notes_off_v(v);
    cfg_defaults();
    for (v = 0; v < ENG_VOICES; v++) {
        mix_vol[v] = 100;
        mix_mute[v] = 0;
        AV[v].play = 0;
        AV[v].cfg = (arp_cfg_t)ARP_CFG_DEFAULT;
        AV[v].dirty = 1;
    }
    mix_vol[DRUM_V] = 100;
    mix_mute[DRUM_V] = 0;
    dr_defaults();
    mix_apply_all();
    octave = 0;
    arp_p = &AV[0];
    dsel = 0;
    eng_voice_select(0);
    voices_boot(0u);
    leds_avail();
    leds_octave();
    leds_arp();
    if (flash_ok) {
        eng_run = 0;
        (void)ps_erase();
        eng_run = 1;
    }
    ui_msg("DEFAULTS", 1500);
}

static uint8_t midi_clk_run = 1;                /* MIDI clock passes until a Stop (0xFC), and again after Start / Continue */

static void midi_drain(void)                    /* USB-MIDI events -> the engine */
{
    while (mi_r != mi_w) {
        uint32_t pkt = midi_in_q[mi_r % MQ], st = (pkt >> 8) & 0xF0u, n = (pkt >> 16) & 0x7Fu, v = pkt >> 24;
        RING_PUBLISH();
        mi_r++;
        uint32_t chn = ((pkt >> 8) & 15u) + 1u, mv, redraw = 0;
        if (st == 0xF0u) {                      /* realtime: the clock comes from the arpeggiator while it runs, else from MIDI in */
            uint32_t rt = (pkt >> 8) & 0xFFu;
            if (rt == 0xF8u) {
                if (midi_clk_run && cfg.clock_in) {
                    dr_ext_ticks++;                 /* (the drum machine follows it while it comes) */
                    if (!(AV[0].play | AV[1].play))
                        eng_clock_tick();
                }
            } else {
                midi_clk_run = rt != 0xFCu;
            }
        } else {                                /* channel messages: each voice listens to its own channel (MENU > V1 / V2 CHANNEL) */
            for (mv = 0; mv < ENG_VOICES; mv++) {
                if (cfg.ch[mv] && cfg.ch[mv] != chn)
                    continue;
                if (st == 0x90u && v) {
                    eng_note_on_v(mv, n, (int32_t)(v * 65536u / 127u));
                } else if (st == 0x80u || st == 0x90u) {
                    eng_note_off_v(mv, n);
                } else if (st == 0xB0u && n == 123u) {
                    eng_all_notes_off_v(mv);
                } else if (st == 0xC0u && cfg.prog_chg) {   /* program change: a preset of the list, on that voice */
                    uint32_t keep = eng_voice();
                    eng_voice_select(mv);
                    preset_load(n);
                    eng_voice_select(keep);
                    redraw = 1;
                }
            }
            if (!cfg.ch[DRUM_V] || cfg.ch[DRUM_V] == chn) {     /* the drums: GM drum notes, program change = pattern */
                if (st == 0x90u && v) {
                    dr_hit((uint32_t)dr_midi_part(n), v);       /* (-1: no part, ignored) */
                } else if (st == 0xC0u && cfg.prog_chg) {
                    dr_load_preset(n);
                    redraw = 1;
                }
            }
            if (redraw)
                screen_idle();
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
    if (page == PAGE_MENU) {                        /* the settings menu: ALGO tabs, PRESET rows, K1..K4 the value */
        menu_knobs(st);
        return;
    }
    if (st[6]) {
        if (dsel)
            dr_pattern_step(st[6]);                 /* the drums: the factory patterns */
        else
            preset_step(st[6]);
        return;
    }
    if (page == PAGE_MIXER) {                       /* K1 / K2 / K3: the level of voice 1 / 2 / the drums */
        for (k = 0; k < PVOICES; k++) {
            uint32_t fast = cfg.accel && fm1_ms - last_ms[k] < 80u;
            if (st[2u + k]) {
                last_ms[k] = fm1_ms;
                mix_turn(k, st[2u + k], (int)fast);
            }
        }
        if (st[0])
            page_set(page);                         /* SELECT: back to the module page */
        return;
    }
    if (dsel) {                                     /* the drum page: SELECT the part, K1..K4 */
        dr_knobs(st);
        return;
    }
    if (page && st[0])                              /* SELECT: back to the module page */
        page_set(page);
    if (page) {                                     /* K1..K4 of an arpeggiator page */
        for (k = 0; k < 4u; k++) {
            uint32_t fast = cfg.accel && fm1_ms - last_ms[k] < 80u;
            if (st[2u + k] && page == 1u && k == 1u && eng_voice() != 0u) {   /* the scale is set on voice 1 only */
                ui_msg("SCALE: VOICE 1", 1000);
            } else if (st[2u + k]) {
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
            uint32_t fast = cfg.accel && fm1_ms - last_ms[k] < 80u;
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
            if (cfg.accel && inc < ENG_ONE && fm1_ms - last_ms[k] < 80u)
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
    static uint32_t prev, prev_raw, ignore;
    uint32_t raw = fm1_in.notes, now, diff, n;
    if (page == PAGE_MIXER) {                       /* the mixer: the F# and G# keys mute voice 1 / 2 and play nothing */
        uint32_t rise = raw & ~prev_raw & MIX_KEYS, v;
        for (v = 0; v < PVOICES; v++)
            if (rise >> MIX_KEY_BIT[v] & 1u)
                mix_toggle(v);
        ignore |= rise;                             /* (until they are let go, also after leaving the page) */
    }
    prev_raw = raw;
    ignore &= raw;
    now = raw & ~ignore;
    if (dsel) {                                     /* the drums: the keys are step buttons (drum_ui.c), they play no notes */
        if (!page)
            dr_keys(now);
        prev = 0;
        return;
    }
    diff = now ^ prev;
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
#define BTN_SEL 3u                  /* switches between voice 1 and 2 */
#define BTN_GLO 7u                  /* the mixer page */
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
    int run = dsel ? dr_play : arp_play;
    fm1_led_key(BTN_PLAY, !run);                    /* stopped: PLAY's red LED; running: the green one breathes */
    fm1_led[8] &= (uint8_t)~2u;
    if (run)
        fm1_led_breath[8] |= 2u;
    else
        fm1_led_breath[8] &= (uint8_t)~2u;
    fm1_led_key(BTN_ARP, page == 1u);
    fm1_led_key(BTN_SEQ, page == 2u);
    fm1_led_key(BTN_LFO, page == 3u);
    fm1_led_key(BTN_SEL, dsel || eng_voice() != 0u);   /* lit while another voice than 1 is selected */
    fm1_led_key(BTN_GLO, page == PAGE_MIXER);
}

static void page_set(uint32_t p)    /* the same button again goes back to the module page */
{
    page = (uint8_t)(page == p ? 0u : p);
    if (page == PAGE_MENU)
        menu_open();
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
    if (!flash_ok || !m1_cur_lenv[0]) {
        ui_msg("NO FLASH", 1500);
        return;
    }
    ui_msg("SAVING", 5000);
    ui_msg(m1_store(m1_curv[0], m1_cur_lenv[0]) ? "ERROR" : "SAVED", 1500);   /* voice 1's patch; voice 2 only as its preset */
}

/* The panel buttons (matrix ids as in Felucca's panel table; unknown ones are shown so they can be assigned):
 * OCT- 0, OCT+ 1: octave; SAVE 9: save the state. */
#define BTN_OCT_DOWN 0u
#define BTN_OCT_UP 1u
#define BTN_SAVE 9u
/* the panel's buttons (ids 0..13); the unused ones do nothing yet and show no banner */
#define BTN_FX 2u
#define BTN_ENV 4u
#define BTN_EDIT 6u
#define BTN_REC 13u
#define BTN_HOME 8u                     /* HOME: back to the module page, on the FM-1 panel module of the patch */

static void led_glow(uint32_t id, int on)           /* the HAL's dim glow (~1/30 of lit) for a button's LED */
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id) {
                if (on)
                    fm1_led_dim[p] |= (uint8_t)(1u << r);
                else
                    fm1_led_dim[p] &= (uint8_t)~(1u << r);
            }
}

static void leds_avail(void)                        /* the page buttons glow dimly (lit while their page shows); MENU > LEDS */
{
    int on = cfg.leds != 0;
    fm1_led_dim_level(cfg.leds == 2u ? 2u : 0u);  /* DIM LOW is the old glow (~1/30), DIM HIGH a brighter one (~1/12) */
    led_glow(BTN_HOME, on);
    led_glow(BTN_ARP, on);
    led_glow(BTN_SEQ, on);
    led_glow(BTN_LFO, on);
    led_glow(BTN_SEL, on);
    led_glow(BTN_GLO, on);
}

static void home_press(void)
{
    uint32_t n = dsel ? 0u : (uint32_t)eng_nmodules(), i;
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
/* SEL: the next voice (1 -> 2 -> the drums -> 1). The keys, MIDI, knobs and LCD follow it; the notes held on the old voice end */
static void voice_switch(void)
{
    uint32_t v = ((dsel ? DRUM_V : eng_voice()) + 1u) % PVOICES;
    if (!dsel)
        eng_all_notes_off_v(eng_voice());
    if (v == DRUM_V) {
        dsel = 1;
        if (page && page < PAGE_MIXER)              /* (the arpeggiator pages are for the patch voices) */
            page = 0;
    } else {
        dsel = 0;
        eng_voice_select(v);
        arp_p = &AV[v];                             /* the arpeggiator pages and PLAY act on this voice's */
        arp_sync_scale();
    }
    leds_arp();
    lcd_fill(0, 24, 240, 216, C_BG);
    screen_idle();
    {
        char m[20], *q = m;
        q = put(q, "VOICE ");
        *q++ = (char)('1' + v);
        if (dsel)
            q = put(q, " DRUMS");
        *q = 0;
        ui_msg(m, 1000);
    }
}

static void buttons_poll(void)
{
    static uint32_t home_t0;                        /* fm1_ms | 1 of the HOME press, 0: not held; bit 1: the hold fired */
    uint32_t rel, p = fm1_input_edges(&rel);
    if (page == PAGE_MENU) {                        /* the menu: OCT- / OCT+ step the selected row's value */
        if (p & (1u << BTN_OCT_DOWN))
            menu_set_row(menu_sel, -1);
        if (p & (1u << BTN_OCT_UP))
            menu_set_row(menu_sel, 1);
        p &= ~((1u << BTN_OCT_DOWN) | (1u << BTN_OCT_UP));
    }
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
    if (p & (1u << BTN_SEL))
        voice_switch();
    if (p & (1u << BTN_HOME))
        home_t0 = fm1_ms | 1u;                      /* HOME acts on release: a tap goes home, 0.7 s held opens the menu */
    if (home_t0 && !(home_t0 & 2u) && (fm1_in.buttons >> BTN_HOME & 1u) && fm1_ms - (home_t0 & ~3u) > 700u) {
        home_t0 |= 2u;
        page_set(PAGE_MENU);                        /* (closes it when it is open) */
    }
    if (rel & (1u << BTN_HOME)) {
        if (home_t0 && !(home_t0 & 2u)) {
            if (page == PAGE_MENU)
                page_set(PAGE_MENU);                /* a tap closes the menu */
            else
                home_press();
        }
        home_t0 = 0;
    }
    if (p & (1u << BTN_GLO))
        page_set(PAGE_MIXER);
    if (dsel && (p & ((1u << BTN_ARP) | (1u << BTN_SEQ) | (1u << BTN_LFO))))
        ui_msg("NO ARP ON DRUMS", 1000);
    else if (p & (1u << BTN_ARP))
        page_set(1);
    else if (p & (1u << BTN_SEQ))
        page_set(2);
    else if (p & (1u << BTN_LFO))
        page_set(3);
    if (p & (1u << BTN_PLAY)) {
        if (dsel)
            dr_toggle_play();
        else
            arp_toggle();
    }
}

static void uboot_now(void)                             /* the update loader (also MENU > UPDATE MODE) */
{
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, C_BG);
    text5x7(12, 110, "UBOOT", 4, C_ACC, C_BG);
    usb_detach();
    fm1_delay_ms(30);
    bootguard.pending = 0;
    fm1_enter_uboot();
}

/* USB host requests: the editor's SysEx (proto.c) before the update protocol's, the M-UPGRADE handshake and upgrade, the
 * UBOOT SysEx. Run by the main loop and while the loading screen waits, so a host is answered from the start. */
static void host_poll(void)
{
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

static void fm1_main(void)
{
    uint32_t t_btn = 0;
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;     /* the expected 1 MiB part, else no OTA staging */
    if (flash_ok)
        ota_boot_cleanup();                             /* staging area left by an update */
    lcd_init();
    splash_draw();                                      /* the loading screen: until the patches are up, and at least SPLASH_MS */
    t_splash = fm1_ms;
    boot_step("INPUT");
    fm1_input_init();
    fm1_adc_init();
    master_boot();
    boot_step("AUDIO");
    audio_init();
    boot_step("USB");
    usb_start();
    boot_step("MIDI");
    uart_midi_init();                                   /* before TIMER5: PORTH read-modify-write */
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    fm1_delay_ms(30);
    boot_step("STATE");
    dr_defaults();
    patch_boot();
    while (fm1_ms - t_splash < SPLASH_MS) {             /* the screen stays up; the bar fills with the time; MIDI and the host */
        fm1_wdt_feed();                                 /* are served meanwhile (notes play, the editor and flasher get answers) */
        usb_retry(fm1_ms);
        midi_drain();
        host_poll();
        boot_step(BOOT_READY);
    }
    booting = 0;
    lcd_fill(0, 0, 240, 240, C_BG);
    screen_idle();                                      /* the top bar and the page */
    if (boot_msg)
        ui_msg(boot_msg, boot_msg_ms);
    t_btn = fm1_ms;                                     /* the OCT- + OCT+ hold counts from here, not from power-up */
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
            dr_step_poll();
            cpu_poll();
        }
        {   /* OCT- + OCT+ (matrix buttons 0 and 1) held 5 s: UBOOT, as in Felucca */
            uint32_t both = 3u;
            if ((fm1_in.buttons & both) != both)
                t_btn = fm1_ms;
            else if (fm1_ms - t_btn > 5000u)
                uboot_now();
        }
        host_poll();
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
