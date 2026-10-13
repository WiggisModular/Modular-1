/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 morgulbrut */
/* The preset bank: 32 slots of one 4 KiB flash sector each (0xA0000 .. 0xC0000, in Felucca's data region and clear of
 * the current-patch sectors of patchstore.c). A slot is a 48-byte header at the start (magic, slot, length, CRC-16,
 * name; written last, so it is the commit record) and the patch from offset 256. A slot whose header or CRC does not
 * check is empty. Needs st_read / st_erase / st_prog (storage_hw.c) and m1_crc16 (proto.c). */
#define SL_BASE 0xA0000u
#define SL_MAGIC 0x4C53314Du            /* "M1SL" */
#define SL_PAYLOAD 256u

typedef struct {
    uint32_t magic, slot, len, crc;
    char name[M1_NAME];
    uint32_t rsv[3];
} sl_hdr_t;
_Static_assert(sizeof(sl_hdr_t) == 48u, "slot header");

static uint8_t sl_buf[M1_PATCH_MAX] ENG_POOL;
static uint32_t bank_mask;                       /* bit n: slot n holds a preset */

static uint32_t sl_addr(uint32_t slot) { return SL_BASE + slot * 0x1000u; }

/* header of a slot if it is valid; with check != 0 also the payload's CRC (the payload goes to sl_buf) */
static int sl_head(uint32_t slot, sl_hdr_t *h, int check)
{
    if (st_read(sl_addr(slot), h, sizeof *h) || h->magic != SL_MAGIC || h->slot != slot || h->len < 8u || h->len > M1_PATCH_MAX)
        return -1;
    if (check && (st_read(sl_addr(slot) + SL_PAYLOAD, sl_buf, h->len) || m1_crc16(sl_buf, h->len) != h->crc))
        return -1;
    return 0;
}

static void sl_scan(void)
{
    sl_hdr_t h;
    uint32_t s;
    bank_mask = 0;
    for (s = 0; s < M1_SLOTS; s++) {
        fm1_wdt_feed();
        if (sl_head(s, &h, 1) == 0)
            bank_mask |= 1u << s;
    }
}

/* the preset in a slot into dst; 0 = ok */
static int sl_read(uint32_t slot, uint8_t *dst, uint32_t *len, char *name)
{
    sl_hdr_t h;
    uint32_t i;
    if (slot >= M1_SLOTS || sl_head(slot, &h, 1))
        return -1;
    for (i = 0; i < h.len; i++)
        dst[i] = sl_buf[i];
    *len = h.len;
    for (i = 0; i < M1_NAME; i++)
        name[i] = h.name[i];
    name[M1_NAME] = 0;
    return 0;
}

static int sl_write(uint32_t slot, const char *name, const uint8_t *p, uint32_t len)
{
    sl_hdr_t h;
    uint32_t i;
    int rc;
    h.magic = SL_MAGIC;
    h.slot = slot;
    h.len = len;
    h.crc = m1_crc16(p, len);
    for (i = 0; i < M1_NAME; i++)
        h.name[i] = name[i];
    h.rsv[0] = h.rsv[1] = h.rsv[2] = 0;
    rc = st_erase(sl_addr(slot));
    if (!rc)
        rc = st_prog(sl_addr(slot) + SL_PAYLOAD, p, len);
    if (!rc)
        rc = st_prog(sl_addr(slot), (const uint8_t *)&h, sizeof h);          /* the commit record, last */
    if (!rc && sl_head(slot, &h, 1))
        rc = -1;
    if (!rc)
        bank_mask |= 1u << slot;
    else
        bank_mask &= ~(1u << slot);
    return rc;
}

static int sl_erase(uint32_t slot)
{
    int rc = 0;
    sl_hdr_t h;
    if (st_read(sl_addr(slot), &h, sizeof h) == 0 && h.magic == 0xFFFFFFFFu)
        rc = 0;                                  /* already blank: no erase */
    else
        rc = st_erase(sl_addr(slot));
    if (!rc)
        bank_mask &= ~(1u << slot);
    return rc;
}
