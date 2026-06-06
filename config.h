/*
 * Copyright (c) 2026 Krzysztof Gawryś
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * config.h - CH32X035 DFU bootloader configuration.
 *
 * IMPORTANT: the flash/RAM addresses here must match the linker scripts:
 *   - ld/boot.ld  : bootloader   FLASH ORIGIN/LENGTH  (BOOT_BASE / BOOT_SIZE)
 *   - ld/app.ld   : application   FLASH ORIGIN/LENGTH  (APP_BASE / APP_SIZE)
 *   - both shrink RAM by one word so BOOT_FLAG_ADDR is reserved.
 */
#ifndef CONFIG_H
#define CONFIG_H

/* ---- Flash layout (CH32X035: 62 KB user flash, 256-byte pages) ---- */
#define BOOT_BASE        0x00000000u
#define BOOT_SIZE        0x2000u                 /* 8 KB reserved for bootloader */
#define APP_BASE         0x00002000u             /* = BOOT_BASE + BOOT_SIZE      */
#define APP_SIZE         (0xF800u - APP_BASE)    /* up to end of 62 KB flash     */

/* ---- RAM hand-off flag (survives a warm reset; outside managed RAM) ---- */
/* RAM is 20 KB (0x20000000..0x20005000); the linkers cap usable RAM at
 * 0x4FF0 so this top word is never touched by .data/.bss or the stack. */
#define BOOT_FLAG_ADDR   0x20004FF0u
#define BOOT_MAGIC_DFU   0xB0071DF0u             /* "enter DFU at next reset" */

/* ---- DFU entry triggers ---- */
/* 1: enter DFU automatically when the app region looks empty/invalid. */
#define DFU_ENTER_IF_NO_APP   1
/* 1: enter DFU when a button/jumper is held at reset (independent of the app). */
#define DFU_BUTTON_ENABLE     0
#define DFU_BUTTON_PORT       GPIOC
#define DFU_BUTTON_PIN        GPIO_Pin_0
#define DFU_BUTTON_RCC        RCC_APB2Periph_GPIOC
#define DFU_BUTTON_ACTIVE_LOW 1                  /* pressed = pin reads 0 */

/* ---- USB identity ---- */
/* NOTE: 0x1A86 is WCH's USB-IF Vendor ID. Fine for private/hobby use; for a
 * published product use your own VID or a pid.codes test allocation. */
#define USB_VID          0x1A86u
#define USB_PID          0x8035u
#define USB_STR_MANUF    "wch.cn"
#define USB_STR_PRODUCT  "CH32X035 DFU"
#define USB_STR_SERIAL   "0001"
#define USB_STR_IFACE    "App @0x2000"

/* DFU full-speed transfer size: keep 64 (= EP0 max packet) so each block is a
 * single control-OUT packet; the bootloader buffers blocks into 256-B pages. */
#define DFU_TRANSFER_SIZE     64

/* Busy-loop iterations to wait after the host reads the post-manifest status,
 * before rebooting into the app (lets dfu-util finish cleanly; USB IRQs keep
 * being serviced during the wait). ~1 s at 48 MHz. Calibration-independent on
 * purpose - Delay_Ms can be mis-scaled depending on the clock setup. */
#define DFU_MANIFEST_REBOOT_LOOPS  12000000u

#endif /* CONFIG_H */
