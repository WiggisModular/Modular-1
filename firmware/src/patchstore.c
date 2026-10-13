/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* The current patch in flash, power-fail safe: two 4 KiB sectors (0x97000, 0x98000), each a 32-byte header at the
 * start (written last: the commit record) and the patch from offset 256. The valid copy with the newest sequence
 * number wins; a save always goes to the other sector, so a write torn at any point leaves the old patch.
 * Needs st_read / st_erase / st_prog (storage_hw.c) and m1_crc16 (proto.c). The area is Felucca's data region;
 * the magic differs from Felucca's, so its store ignores these sectors. */
#define PS_A 0x97000u
#define PS_B 0x98000u
#define PS_MAGIC 0x5450314Du            /* "M1PT" */
#define PS_PAYLOAD 256u
#define PS_MAX (4096u - PS_PAYLOAD)      /* the state blob (main.c): both voices' patches, the arpeggiators */

typedef struct {
    uint32_t magic, seq, len, crc, rsv[4];
} ps_hdr_t;

static uint8_t ps_buf[PS_MAX] ENG_POOL;
static uint32_t ps_state[4];                     /* state saved with the patch: octave + 8, selected module, preset index, voice 2's preset + 1 */
static uint32_t ps_loaded[4];                    /* the state of the patch ps_load found */

/* the sector's patch into ps_buf if it is valid */
static int ps_check(uint32_t base, ps_hdr_t *h)
{
    if (st_read(base, h, sizeof *h) || h->magic != PS_MAGIC || h->len < 8u || h->len > PS_MAX)
        return -1;
    if (st_read(base + PS_PAYLOAD, ps_buf, h->len) || m1_crc16(ps_buf, h->len) != h->crc)
        return -1;
    return 0;
}

/* the newest valid patch into dst (max PS_MAX); 0 = found. *seq: its sequence number, *sector: which */
static int ps_load(uint8_t *dst, uint32_t *len, uint32_t *seq, uint32_t *sector)
{
    ps_hdr_t a, b;
    int va = ps_check(PS_A, &a) == 0, vb;
    uint32_t i;
    if (va)
        for (i = 0; i < a.len; i++)
            dst[i] = ps_buf[i];
    vb = ps_check(PS_B, &b) == 0;                       /* (ps_buf now holds B if valid) */
    if (!va && !vb)
        return -1;
    if (vb && (!va || (int32_t)(b.seq - a.seq) > 0)) {
        for (i = 0; i < b.len; i++)
            dst[i] = ps_buf[i];
        *len = b.len;
        *seq = b.seq;
        *sector = PS_B;
        for (i = 0; i < 4u; i++)
            ps_loaded[i] = b.rsv[i];
    } else {
        *len = a.len;
        *seq = a.seq;
        *sector = PS_A;
        for (i = 0; i < 4u; i++)
            ps_loaded[i] = a.rsv[i];
    }
    return 0;
}

static int ps_save(const uint8_t *p, uint32_t len)
{
    static uint8_t cur[PS_MAX] ENG_POOL;
    ps_hdr_t h;
    uint32_t seq = 0, sector = PS_B, clen, target;
    int rc;
    if (len < 8u || len > PS_MAX)
        return -1;
    if (ps_load(cur, &clen, &seq, &sector) == 0)
        target = sector == PS_A ? PS_B : PS_A;
    else
        target = PS_A, seq = 0;
    h.magic = PS_MAGIC;
    h.seq = seq + 1u;
    h.len = len;
    h.crc = m1_crc16(p, len);
    h.rsv[0] = ps_state[0];
    h.rsv[1] = ps_state[1];
    h.rsv[2] = ps_state[2];
    h.rsv[3] = ps_state[3];
    rc = st_erase(target);
    if (!rc)
        rc = st_prog(target + PS_PAYLOAD, p, len);
    if (!rc)
        rc = st_prog(target, (const uint8_t *)&h, sizeof h);          /* the commit record, last */
    if (!rc && ps_check(target, &h))
        rc = -1;
    return rc;
}

static int ps_erase(void)                               /* the stored state away: the next boot runs the defaults */
{
    int a = st_erase(PS_A), b = st_erase(PS_B);
    return a | b;
}
