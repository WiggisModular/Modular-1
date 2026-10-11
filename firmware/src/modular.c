/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Modified for Modular-1 (morgulbrut). */
/* Modular-1 bring-up firmware: one compilation unit (the HAL is header-only). Boots on Felucca's HAL, shows its
 * identity on the LCD, plays a sine on MIDI notes, and keeps Felucca's USB-MIDI + OTA path so it can be re-flashed. */

#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_800"    /* package identity (build.py: the .fwsc marker string) */
#endif
#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "M1-0.9"
#endif
#define FELUCCA_FLASH 1
#define FELUCCA_OTA 1
#define FELUCCA_OTA_DRYRUN 0
#define FELUCCA_CDC 0
#define FELUCCA_UAC 0
#define FELUCCA_UART 0
#define FELUCCA_UAC_TONE 0

#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_audio.h"
#include "fm1_adc.h"
#include "fm1_flash.h"
#include "fm1_lcd_hw.h"
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")

static volatile uint32_t fm1_ms;                   /* milliseconds since boot (TIMER5 ISR) */
struct { uint32_t magic, failed, pending; } bootguard __attribute__((section(".noinit")));
#define BOOTGUARD_MAGIC 0x42475244u

#include "libc.c"
#include "lcd.c"
#include "font5x7.c"
#define ENG_POOL __attribute__((section(".pool")))
#include "engine.c"
#include "presets.h"
#include "arp.c"
#include "audio.c"
#include "usb.c"
#include "storage_hw.c"
#include "ota.c"
#include "ota_hw.c"
static int m1_apply(const uint8_t *patch, uint32_t len);       /* main.c: the hooks proto.c needs */
static int m1_store(const uint8_t *patch, uint32_t len);
static int m1_flash_ok(void);
static void m1_send(const uint8_t *f, uint32_t n);
static int m1_slot_write(uint32_t slot, const char *name, const uint8_t *p, uint32_t len);
static int m1_slot_erase(uint32_t slot);
static int m1_slot_info(uint32_t slot, char *name, uint32_t *len);
#include "proto.c"
#include "patchstore.c"
#include "slotstore.c"
#ifdef M1_BENCH
#include "bench.c"
#endif
#include "ui.c"
#include "arp_ui.c"
#include "main.c"
