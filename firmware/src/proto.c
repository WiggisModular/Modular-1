/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* SysEx protocol of the editor (web/src/protocol.ts documents the wire format): patch upload in chunks, commit,
 * store to flash, live parameter changes. Self-contained: the includer provides
 *   static int  m1_apply(const uint8_t *patch, uint32_t len);   switch the engine to a patch, 0 = ok, else eng_load's error
 *   static int  m1_store(const uint8_t *patch, uint32_t len);   write it to flash, 0 = ok
 *   static int  m1_flash_ok(void);
 *   static void m1_send(const uint8_t *f, uint32_t n);          a full F0 .. F7 message to the host
 *   static int  m1_slot_write(uint32_t slot, const char *name, const uint8_t *p, uint32_t len);   preset bank: 0 = ok
 *   static int  m1_slot_erase(uint32_t slot);
 *   static int  m1_slot_info(uint32_t slot, char *name, uint32_t *len);                          1 = the slot holds a preset
 * and calls m1_handle() with every received frame (the bytes between F0 and F7). */
#include "engine.h"
#include "defs.h"

#define M1_PATCH_MAX 3072u
#define M1_VERSION 1

enum { C_HELLO = 0x01, C_CHUNK = 0x10, C_COMMIT = 0x11, C_STORE = 0x12, C_PARAM = 0x20, C_ATT = 0x21, C_SLOT = 0x13, C_SLOT_ERASE = 0x14, C_SLOT_INFO = 0x15, R_ACK = 0x40, R_INFO = 0x41, R_SLOT = 0x42 };
#define M1_SLOTS 32u
#define M1_NAME 20u
enum { S_OK, S_RANGE, S_CRC, S_LOAD, S_FLASH, S_CMD, S_NONE };

static uint8_t m1_up[M1_PATCH_MAX], m1_cur[M1_PATCH_MAX];
static uint32_t m1_up_got, m1_up_total, m1_cur_len;

static uint16_t m1_crc16(const uint8_t *d, uint32_t n)       /* CRC-16/CCITT-FALSE */
{
    uint32_t c = 0xFFFFu, i, k;
    for (i = 0; i < n; i++) {
        c ^= (uint32_t)d[i] << 8;
        for (k = 0; k < 8; k++)
            c = c & 0x8000u ? ((c << 1) ^ 0x1021u) & 0xFFFFu : (c << 1) & 0xFFFFu;
    }
    return (uint16_t)c;
}

static uint32_t m1_unpack7(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t max)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)(*in++ & 0x7Fu) << nb;
        nb += 7u;
        if (nb >= 8u) {
            if (o < max)
                out[o] = (uint8_t)acc;
            o++;
            acc >>= 8;
            nb -= 8u;
        }
    }
    return o;
}

static uint32_t m1_pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        nb += 8u;
        while (nb >= 7u) {
            out[o++] = (uint8_t)(acc & 0x7Fu);
            acc >>= 7;
            nb -= 7u;
        }
    }
    if (nb)
        out[o++] = (uint8_t)(acc & 0x7Fu);
    return o;
}

static void m1_reply(uint32_t cmd, const uint8_t *payload, uint32_t n)
{
    uint8_t f[48];
    uint32_t w = 0;
    f[w++] = 0xF0;
    f[w++] = 0x7D;
    f[w++] = 0x4D;
    f[w++] = 0x31;
    f[w++] = (uint8_t)cmd;
    w += m1_pack7(payload, n, f + w);
    f[w++] = 0xF7;
    m1_send(f, w);
}

static void m1_ack(uint32_t cmd, uint32_t seq, uint32_t status, uint32_t aux)
{
    uint8_t p[4];
    p[0] = (uint8_t)cmd;
    p[1] = (uint8_t)seq;
    p[2] = (uint8_t)status;
    p[3] = (uint8_t)aux;
    m1_reply(R_ACK, p, 4);
}

/* a patch the firmware already runs (boot: from flash or a factory patch): what STORE writes and PARAM edits */
static void m1_set_current(const uint8_t *p, uint32_t len)
{
    uint32_t i;
    if (len > M1_PATCH_MAX)
        return;
    for (i = 0; i < len; i++)
        m1_cur[i] = p[i];
    m1_cur_len = len;
}

/* byte offset of module `mod`'s record in the patch binary, 0 if the binary is malformed */
static uint32_t m1_record(uint32_t mod)
{
    uint32_t at = 8, i;
    if (m1_cur_len < 8u || mod >= m1_cur[2])
        return 0;
    for (i = 0; i < mod; i++) {
        const def_t *d;
        if (at >= m1_cur_len || m1_cur[at] >= N_TYPES)
            return 0;
        d = &DEFS[m1_cur[at]];
        at += 1u + 4u * d->npar + (d->cvbase >= 0 ? 14u : 0u);
    }
    return at < m1_cur_len && m1_cur[at] < N_TYPES ? at : 0;
}

static void m1_put32(uint32_t at, int32_t v)
{
    m1_cur[at] = (uint8_t)v;
    m1_cur[at + 1] = (uint8_t)((uint32_t)v >> 8);
    m1_cur[at + 2] = (uint8_t)((uint32_t)v >> 16);
    m1_cur[at + 3] = (uint8_t)((uint32_t)v >> 24);
}

/* the stored copy follows live edits: write the value into the binary patch */
static void m1_patch_param(uint32_t mod, uint32_t par, int32_t v)
{
    uint32_t at = m1_record(mod);
    if (!at || par >= DEFS[m1_cur[at]].npar)
        return;
    m1_put32(at + 1u + 4u * par, v);
}

static void m1_patch_att(uint32_t mod, uint32_t input, int32_t v)
{
    uint32_t at = m1_record(mod);
    if (!at || input >= 2u || DEFS[m1_cur[at]].cvbase < 0)
        return;
    m1_put32(at + 1u + 4u * DEFS[m1_cur[at]].npar + 4u * input, v);
}

/* a patch binary that eng_load would at least not reject for its framing: header, record sizes add up to the length */
static int m1_valid(const uint8_t *p, uint32_t len)
{
    uint32_t at = 8, i, nmod, ncab;
    if (len < 8u || p[0] != 0xA1 || p[1] != ENG_PATCH_VERSION)
        return 0;
    nmod = p[2];
    ncab = (uint32_t)p[4] | (uint32_t)p[5] << 8;
    if (nmod > 32u || ncab > 192u)
        return 0;
    for (i = 0; i < nmod; i++) {
        const def_t *d;
        if (at >= len || p[at] >= N_TYPES)
            return 0;
        d = &DEFS[p[at]];
        at += 1u + 4u * d->npar + (d->cvbase >= 0 ? 14u : 0u);
    }
    return at + 4u * ncab == len;
}

/* f: the bytes between F0 and F7. Returns 1 if the frame was ours (and is consumed). */
static int m1_handle(const uint8_t *f, uint32_t n)
{
    uint8_t d[M1_PATCH_MAX > 512u ? 512u : M1_PATCH_MAX];
    uint32_t len;
    if (n < 4u || f[0] != 0x7D || f[1] != 0x4D || f[2] != 0x31)
        return 0;
    len = m1_unpack7(f + 4, n - 4u, d, sizeof d);
    if (len > sizeof d)
        return 1;                                            /* more than any of our frames carries */
    switch (f[3]) {
    case C_HELLO: {
        uint8_t p[5];
        p[0] = M1_VERSION;
        p[1] = 32;                                           /* ENG_MAX_MODULES */
        p[2] = 192;                                          /* ENG_MAX_CABLES */
        p[3] = (uint8_t)((m1_flash_ok() ? 1u : 0u) | (m1_cur_len ? 2u : 0u));
        p[4] = 2;                                            /* ENG_NDELAY */
        m1_reply(R_INFO, p, 5);
        break;
    }
    case C_CHUNK: {
        uint32_t seq, off, total, data, i;
        if (len < 5u) {
            m1_ack(C_CHUNK, 0, S_RANGE, 0);
            break;
        }
        seq = d[0];
        off = (uint32_t)d[1] | (uint32_t)d[2] << 8;
        total = (uint32_t)d[3] | (uint32_t)d[4] << 8;
        data = len - 5u;
        if (total == 0u || total > M1_PATCH_MAX || off + data > total) {
            m1_ack(C_CHUNK, seq, S_RANGE, 0);
            break;
        }
        if (off == 0u) {                                     /* a new upload */
            m1_up_got = 0;
            m1_up_total = total;
        }
        if (total != m1_up_total || off > m1_up_got) {       /* a chunk was lost: the host resends from m1_up_got */
            m1_ack(C_CHUNK, seq, S_RANGE, 0);
            break;
        }
        for (i = 0; i < data; i++)                           /* (a repeated chunk overwrites the same bytes) */
            m1_up[off + i] = d[5 + i];
        if (off + data > m1_up_got)
            m1_up_got = off + data;
        m1_ack(C_CHUNK, seq, S_OK, 0);
        break;
    }
    case C_COMMIT: {
        uint32_t total, crc;
        int rc;
        if (len < 4u) {
            m1_ack(C_COMMIT, 0, S_RANGE, 0);
            break;
        }
        total = (uint32_t)d[0] | (uint32_t)d[1] << 8;
        crc = (uint32_t)d[2] | (uint32_t)d[3] << 8;
        if (total != m1_up_total || total != m1_up_got || total < 8u) {
            m1_ack(C_COMMIT, 0, S_RANGE, 0);
            break;
        }
        if (m1_crc16(m1_up, total) != crc) {
            m1_ack(C_COMMIT, 0, S_CRC, 0);
            break;
        }
        rc = m1_apply(m1_up, total);
        if (rc) {                                            /* refused: back to the patch that ran before */
            if (m1_cur_len)
                (void)m1_apply(m1_cur, m1_cur_len);
            m1_ack(C_COMMIT, 0, S_LOAD, (uint32_t)-rc);
            break;
        }
        m1_set_current(m1_up, total);
        m1_up_got = m1_up_total = 0;
        m1_ack(C_COMMIT, 0, S_OK, 0);
        break;
    }
    case C_STORE:
        if (!m1_cur_len)
            m1_ack(C_STORE, 0, S_NONE, 0);
        else
            m1_ack(C_STORE, 0, m1_store(m1_cur, m1_cur_len) ? S_FLASH : S_OK, 0);
        break;
    case C_PARAM:
        if (len >= 6u) {
            int32_t v = (int32_t)((uint32_t)d[2] | (uint32_t)d[3] << 8 | (uint32_t)d[4] << 16 | (uint32_t)d[5] << 24);
            eng_set_param(d[0], d[1], v);
            m1_patch_param(d[0], d[1], v);
        }
        break;
    case C_SLOT: {                                           /* the uploaded patch into preset slot n, without loading it */
        uint32_t slot, total, crc, i;
        char name[M1_NAME + 1];
        if (len < 1u + M1_NAME + 4u) {
            m1_ack(C_SLOT, 0, S_RANGE, 0);
            break;
        }
        slot = d[0];
        for (i = 0; i < M1_NAME; i++)
            name[i] = (char)(d[1 + i] >= 32u && d[1 + i] < 127u ? d[1 + i] : 0);
        name[M1_NAME] = 0;
        total = (uint32_t)d[1 + M1_NAME] | (uint32_t)d[2 + M1_NAME] << 8;
        crc = (uint32_t)d[3 + M1_NAME] | (uint32_t)d[4 + M1_NAME] << 8;
        if (slot >= M1_SLOTS || total != m1_up_total || total != m1_up_got || total < 8u) {
            m1_ack(C_SLOT, slot, S_RANGE, 0);
            break;
        }
        if (m1_crc16(m1_up, total) != crc) {
            m1_ack(C_SLOT, slot, S_CRC, 0);
            break;
        }
        if (!m1_valid(m1_up, total)) {
            m1_ack(C_SLOT, slot, S_LOAD, 0);
            break;
        }
        m1_ack(C_SLOT, slot, m1_slot_write(slot, name, m1_up, total) ? S_FLASH : S_OK, 0);
        m1_up_got = m1_up_total = 0;
        break;
    }
    case C_SLOT_ERASE:
        if (len < 1u || d[0] >= M1_SLOTS)
            m1_ack(C_SLOT_ERASE, 0, S_RANGE, 0);
        else
            m1_ack(C_SLOT_ERASE, d[0], m1_slot_erase(d[0]) ? S_FLASH : S_OK, 0);
        break;
    case C_SLOT_INFO: {
        uint8_t r[4 + M1_NAME];
        char name[M1_NAME + 1];
        uint32_t n = 0, i, ok;
        if (len < 1u || d[0] >= M1_SLOTS) {
            m1_ack(C_SLOT_INFO, 0, S_RANGE, 0);
            break;
        }
        for (i = 0; i <= M1_NAME; i++)
            name[i] = 0;
        ok = (uint32_t)m1_slot_info(d[0], name, &n);
        r[0] = d[0];
        r[1] = (uint8_t)ok;
        r[2] = (uint8_t)n;
        r[3] = (uint8_t)(n >> 8);
        for (i = 0; i < M1_NAME; i++)
            r[4 + i] = (uint8_t)name[i];
        m1_reply(R_SLOT, r, sizeof r);
        break;
    }
    case C_ATT:
        if (len >= 6u) {
            int32_t v = (int32_t)((uint32_t)d[2] | (uint32_t)d[3] << 8 | (uint32_t)d[4] << 16 | (uint32_t)d[5] << 24);
            eng_set_att(d[0], d[1], v);
            m1_patch_att(d[0], d[1], v);
        }
        break;
    default:
        m1_ack(f[3], 0, S_CMD, 0);
        break;
    }
    return 1;
}
