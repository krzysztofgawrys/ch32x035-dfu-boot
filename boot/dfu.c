/*
 * Copyright (c) 2026 Krzysztof Gawryś
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * dfu.c - USB DFU 1.1 device for the CH32X035 bootloader (dfu-util compatible).
 *
 * Control endpoint (EP0) only. DFU_DNLOAD blocks (DFU_TRANSFER_SIZE = 64 B,
 * one control-OUT packet each) are buffered into 256-byte flash pages and
 * programmed to the application region via FLASH_ROM_ERASE/WRITE. DFU_UPLOAD
 * reads the region back. After manifestation the device reboots so the
 * bootloader starts the freshly flashed application.
 *
 *   dfu-util -a 0 -d <vid>:<pid> -D app.bin -R     # flash
 *   dfu-util -a 0 -d <vid>:<pid> -U dump.bin       # read back
 *
 * EP0 enumeration mirrors the WCH USBFS SDK CDC example, reduced to EP0.
 */
#include <string.h>
#include "ch32x035.h"
#include "ch32x035_usb.h"
#include "ch32x035_flash.h"
#include "config.h"

/* USBFS endpoint / AFIO bits (from the WCH SDK usbfs device header) */
#define DEF_UEP0         0x00
#define DEF_UEP_IN       0x80
#define USB_IOEN         0x00000080
#define USB_PHY_V33      0x00000040
#define UDP_PUE_MASK     0x0000000C
#define UDP_PUE_1K5      0x0000000C
#define UDM_PUE_MASK     0x00000003

#define UEP0_SIZE        64

/* FLASH_ROM_ERASE/WRITE use the flash alias region (0x08000000), not the
 * execution alias (0x00000000). The app still RUNS from APP_BASE. */
#define FLASH_ALIAS_BASE 0x08000000u
#define DL_BASE          (FLASH_ALIAS_BASE + APP_BASE)
#define DL_END           (DL_BASE + APP_SIZE)

/* DFU class requests */
#define DFU_DETACH       0
#define DFU_DNLOAD       1
#define DFU_UPLOAD       2
#define DFU_GETSTATUS    3
#define DFU_CLRSTATUS    4
#define DFU_GETSTATE     5
#define DFU_ABORT        6

/* DFU states / status */
#define ST_dfuIDLE         2
#define ST_dfuDNLOAD_IDLE  5
#define ST_dfuMANIFEST     7
#define ST_dfuUPLOAD_IDLE  9
#define ST_dfuERROR        10
#define STATUS_OK          0x00
#define STATUS_errADDRESS  0x08
#define STATUS_errWRITE    0x03

/* ---- USB device state ---- */
static volatile uint8_t  dev_addr;
static volatile uint8_t  dev_config;
static const uint8_t    *p_descr;
static uint8_t  req_type, req_code;
static uint16_t req_value, req_len;

__attribute__((aligned(4))) static uint8_t ep0_buf[UEP0_SIZE];
static uint8_t  str_buf[UEP0_SIZE];      /* runtime-built string descriptors */

/* ---- DFU state ---- */
static uint8_t  dfu_state  = ST_dfuIDLE;
static uint8_t  dfu_status = STATUS_OK;
static uint8_t  dfu_io_buf[6];
static volatile uint8_t reboot_armed;

/* ---- flash page accumulator ---- */
__attribute__((aligned(4))) static uint8_t page_buf[256];
static uint32_t dl_addr;
static uint16_t page_len;

/* ============================ Descriptors ============================= */

static const uint8_t dev_descr[] = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, UEP0_SIZE,
    (uint8_t)USB_VID, (uint8_t)(USB_VID >> 8),
    (uint8_t)USB_PID, (uint8_t)(USB_PID >> 8),
    0x00, 0x01, 0x01, 0x02, 0x03, 0x01,
};

static const uint8_t cfg_descr[] = {
    0x09, 0x02, 0x1B, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
    /* DFU interface: class 0xFE / sub 1 / proto 2 (DFU mode), iInterface=4 */
    0x09, 0x04, 0x00, 0x00, 0x00, 0xFE, 0x01, 0x02, 0x04,
    /* DFU functional descriptor: CanDnload|CanUpload|ManifestationTolerant */
    0x09, 0x21, 0x07,
    0xFF, 0x00,
    (uint8_t)DFU_TRANSFER_SIZE, (uint8_t)(DFU_TRANSFER_SIZE >> 8),
    0x10, 0x01,
};

static const uint8_t lang_descr[] = { 0x04, 0x03, 0x09, 0x04 };

/* Build a USB string descriptor (UTF-16LE) from an ASCII string. */
static uint16_t make_string(const char *s)
{
    uint16_t n = 0;
    while (s[n] && (2 + 2 * (n + 1)) <= (int)sizeof(str_buf))
        n++;
    str_buf[0] = (uint8_t)(2 + 2 * n);
    str_buf[1] = 0x03;
    for (uint16_t i = 0; i < n; i++) {
        str_buf[2 + 2 * i] = (uint8_t)s[i];
        str_buf[3 + 2 * i] = 0x00;
    }
    return str_buf[0];
}

/* ============================ Flash helpers ============================ */

static void flush_page(void)
{
    if (page_len == 0)
        return;
    if (dl_addr + 256 <= DL_END) {
        while (page_len < 256)
            page_buf[page_len++] = 0xFF;
        if (FLASH_ROM_ERASE(dl_addr, 256) != FLASH_COMPLETE ||
            FLASH_ROM_WRITE(dl_addr, (uint32_t *)page_buf, 256) != FLASH_COMPLETE) {
            dfu_status = STATUS_errWRITE;
            dfu_state  = ST_dfuERROR;
        }
        dl_addr += 256;
    } else {
        dfu_status = STATUS_errADDRESS;   /* image larger than the app region */
        dfu_state  = ST_dfuERROR;
    }
    page_len = 0;
}

static void dnload_data(const uint8_t *buf, uint16_t n)
{
    if (dfu_state == ST_dfuERROR)
        return;
    for (uint16_t i = 0; i < n; i++) {
        page_buf[page_len++] = buf[i];
        if (page_len == 256)
            flush_page();
    }
    dfu_state = ST_dfuDNLOAD_IDLE;
}

/* ============================ USB bring-up ============================= */

static void usb_endp_init(void)
{
    USBFSD->UEP4_1_MOD = 0;
    USBFSD->UEP2_3_MOD = 0;
    USBFSD->UEP0_DMA   = (uint32_t)ep0_buf;
    USBFSD->UEP0_CTRL_H = USBFS_UEP_R_RES_ACK | USBFS_UEP_T_RES_NAK;
}

static void usb_init(void)
{
    GPIO_InitTypeDef gi = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBFS, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);

    gi.GPIO_Pin = GPIO_Pin_16; gi.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &gi);
    gi.GPIO_Pin = GPIO_Pin_17; gi.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOC, &gi);

    AFIO->CTLR = (AFIO->CTLR & ~(UDP_PUE_MASK | UDM_PUE_MASK)) |
                 USB_PHY_V33 | UDP_PUE_1K5 | USB_IOEN;

    USBFSD->BASE_CTRL = 0x00;
    usb_endp_init();
    USBFSD->DEV_ADDR  = 0x00;
    USBFSD->BASE_CTRL = USBFS_UC_DEV_PU_EN | USBFS_UC_INT_BUSY | USBFS_UC_DMA_EN;
    USBFSD->INT_FG    = 0xFF;
    USBFSD->UDEV_CTRL = USBFS_UD_PD_DIS | USBFS_UD_PORT_EN;
    USBFSD->INT_EN    = USBFS_UIE_SUSPEND | USBFS_UIE_BUS_RST | USBFS_UIE_TRANSFER;
    NVIC_EnableIRQ(USBFS_IRQn);
}

/* ============================ EP0 / DFU ISR ============================ */

void USBFS_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

void USBFS_IRQHandler(void)
{
    uint8_t  intflag = USBFSD->INT_FG;
    uint8_t  intst   = USBFSD->INT_ST;
    uint16_t len;
    uint8_t  err = 0;

    if (intflag & USBFS_UIF_TRANSFER) {
        switch (intst & USBFS_UIS_TOKEN_MASK) {

        case USBFS_UIS_TOKEN_IN:
            if ((intst & (USBFS_UIS_TOKEN_MASK | USBFS_UIS_ENDP_MASK)) ==
                (USBFS_UIS_TOKEN_IN | DEF_UEP0)) {
                if (req_len == 0)
                    USBFSD->UEP0_CTRL_H = (USBFSD->UEP0_CTRL_H & ~USBFS_UEP_R_RES_MASK) |
                                          USBFS_UEP_R_TOG | USBFS_UEP_R_RES_ACK;
                if ((req_type & USB_REQ_TYP_MASK) == USB_REQ_TYP_STANDARD &&
                    req_code == USB_GET_DESCRIPTOR) {
                    len = req_len >= UEP0_SIZE ? UEP0_SIZE : req_len;
                    memcpy(ep0_buf, p_descr, len);
                    req_len -= len;
                    p_descr += len;
                    USBFSD->UEP0_TX_LEN = len;
                    USBFSD->UEP0_CTRL_H ^= USBFS_UEP_T_TOG;
                } else if ((req_type & USB_REQ_TYP_MASK) == USB_REQ_TYP_STANDARD &&
                           req_code == USB_SET_ADDRESS) {
                    USBFSD->DEV_ADDR = (USBFSD->DEV_ADDR & USBFS_UDA_GP_BIT) | dev_addr;
                }
            }
            break;

        case USBFS_UIS_TOKEN_OUT:
            if ((intst & (USBFS_UIS_TOKEN_MASK | USBFS_UIS_ENDP_MASK)) ==
                (USBFS_UIS_TOKEN_OUT | DEF_UEP0)) {
                len = USBFSD->RX_LEN;
                if ((req_type & USB_REQ_TYP_MASK) == USB_REQ_TYP_CLASS &&
                    req_code == DFU_DNLOAD)
                    dnload_data(ep0_buf, len);
                req_len = 0;
                USBFSD->UEP0_TX_LEN = 0;
                USBFSD->UEP0_CTRL_H = (USBFSD->UEP0_CTRL_H & ~USBFS_UEP_T_RES_MASK) |
                                      USBFS_UEP_T_TOG | USBFS_UEP_T_RES_ACK;
            }
            break;

        case USBFS_UIS_TOKEN_SETUP:
            USBFSD->UEP0_CTRL_H = USBFS_UEP_T_TOG | USBFS_UEP_T_RES_NAK |
                                  USBFS_UEP_R_TOG | USBFS_UEP_R_RES_NAK;
            req_type  = ep0_buf[0];
            req_code  = ep0_buf[1];
            req_value = (uint16_t)(ep0_buf[2] | (ep0_buf[3] << 8));
            req_len   = (uint16_t)(ep0_buf[6] | (ep0_buf[7] << 8));
            len = 0;

            if ((req_type & USB_REQ_TYP_MASK) == USB_REQ_TYP_CLASS) {
                switch (req_code) {
                case DFU_DNLOAD:
                    if (req_len == 0) {            /* end of download -> manifest */
                        flush_page();
                        if (dfu_state != ST_dfuERROR)
                            dfu_state = ST_dfuMANIFEST;
                    }
                    break;                         /* data handled in OUT stage */
                case DFU_UPLOAD: {
                    uint32_t src = DL_BASE + (uint32_t)req_value * DFU_TRANSFER_SIZE;
                    uint32_t avail = (src < DL_END) ? (DL_END - src) : 0;
                    uint16_t n = (avail > DFU_TRANSFER_SIZE) ? DFU_TRANSFER_SIZE : (uint16_t)avail;
                    if (req_len > n) req_len = n;
                    p_descr = (const uint8_t *)src;
                    dfu_state = ST_dfuUPLOAD_IDLE;
                    break;
                }
                case DFU_GETSTATUS:
                    dfu_io_buf[0] = dfu_status;
                    dfu_io_buf[1] = 10;            /* bwPollTimeout = 10 ms */
                    dfu_io_buf[2] = 0;
                    dfu_io_buf[3] = 0;
                    dfu_io_buf[4] = dfu_state;
                    dfu_io_buf[5] = 0;
                    p_descr = dfu_io_buf;
                    if (dfu_state == ST_dfuMANIFEST) {
                        dfu_state = ST_dfuIDLE;
                        reboot_armed = 1;   /* host has read manifest status */
                    }
                    break;
                case DFU_GETSTATE:
                    dfu_io_buf[0] = dfu_state;
                    p_descr = dfu_io_buf;
                    break;
                case DFU_CLRSTATUS:
                    dfu_status = STATUS_OK;
                    dfu_state  = ST_dfuIDLE;
                    break;
                case DFU_ABORT:
                    dfu_state = ST_dfuIDLE;
                    break;
                case DFU_DETACH:
                    break;
                default:
                    err = 0xFF;
                    break;
                }
                if (req_code != DFU_UPLOAD) {
                    len = (req_len >= UEP0_SIZE) ? UEP0_SIZE : req_len;
                    if (!err && p_descr)
                        memcpy(ep0_buf, p_descr, len);
                } else if (!err) {
                    len = (req_len >= UEP0_SIZE) ? UEP0_SIZE : req_len;
                    memcpy(ep0_buf, p_descr, len);
                    p_descr += len;
                }
            } else {
                switch (req_code) {
                case USB_GET_DESCRIPTOR:
                    switch ((uint8_t)(req_value >> 8)) {
                    case USB_DESCR_TYP_DEVICE:
                        p_descr = dev_descr; len = sizeof(dev_descr); break;
                    case USB_DESCR_TYP_CONFIG:
                        p_descr = cfg_descr; len = sizeof(cfg_descr); break;
                    case USB_DESCR_TYP_STRING:
                        switch ((uint8_t)(req_value & 0xFF)) {
                        case 0: p_descr = lang_descr; len = sizeof(lang_descr); break;
                        case 1: len = make_string(USB_STR_MANUF);   p_descr = str_buf; break;
                        case 2: len = make_string(USB_STR_PRODUCT); p_descr = str_buf; break;
                        case 3: len = make_string(USB_STR_SERIAL);  p_descr = str_buf; break;
                        case 4: len = make_string(USB_STR_IFACE);   p_descr = str_buf; break;
                        default: err = 0xFF; break;
                        }
                        break;
                    default: err = 0xFF; break;
                    }
                    if (req_len > len)
                        req_len = len;
                    len = (req_len >= UEP0_SIZE) ? UEP0_SIZE : req_len;
                    if (!err) {
                        memcpy(ep0_buf, p_descr, len);
                        p_descr += len;
                    }
                    break;
                case USB_SET_ADDRESS:
                    dev_addr = (uint8_t)(req_value & 0xFF);
                    break;
                case USB_GET_CONFIGURATION:
                    ep0_buf[0] = dev_config;
                    if (req_len > 1) req_len = 1;
                    break;
                case USB_SET_CONFIGURATION:
                    dev_config = (uint8_t)(req_value & 0xFF);
                    break;
                case USB_GET_STATUS:
                    ep0_buf[0] = 0x00; ep0_buf[1] = 0x00;
                    if (req_len > 2) req_len = 2;
                    break;
                case USB_GET_INTERFACE:
                    ep0_buf[0] = 0x00;
                    if (req_len > 1) req_len = 1;
                    break;
                case USB_SET_INTERFACE:
                    break;
                default:
                    err = 0xFF;
                    break;
                }
            }

            if (err == 0xFF) {
                USBFSD->UEP0_CTRL_H = USBFS_UEP_T_TOG | USBFS_UEP_T_RES_STALL |
                                      USBFS_UEP_R_TOG | USBFS_UEP_R_RES_STALL;
            } else if (req_type & DEF_UEP_IN) {
                len = (req_len > UEP0_SIZE) ? UEP0_SIZE : req_len;
                req_len -= len;
                USBFSD->UEP0_TX_LEN = len;
                USBFSD->UEP0_CTRL_H = (USBFSD->UEP0_CTRL_H & ~USBFS_UEP_T_RES_MASK) |
                                      USBFS_UEP_T_TOG | USBFS_UEP_T_RES_ACK;
            } else {
                if (req_len == 0) {
                    USBFSD->UEP0_TX_LEN = 0;
                    USBFSD->UEP0_CTRL_H = (USBFSD->UEP0_CTRL_H & ~USBFS_UEP_T_RES_MASK) |
                                          USBFS_UEP_T_TOG | USBFS_UEP_T_RES_ACK;
                } else {
                    USBFSD->UEP0_CTRL_H = (USBFSD->UEP0_CTRL_H & ~USBFS_UEP_R_RES_MASK) |
                                          USBFS_UEP_R_TOG | USBFS_UEP_R_RES_ACK;
                }
            }
            break;

        default:
            break;
        }
        USBFSD->INT_FG = USBFS_UIF_TRANSFER;
    } else if (intflag & USBFS_UIF_BUS_RST) {
        /* Do NOT reset here on manifest: a host/usbip port reset during the
         * post-download status poll would cut dfu-util off (NO_DEVICE). The
         * reboot is done from the main loop after the host reads the status. */
        dev_config = 0;
        dev_addr = 0;
        USBFSD->DEV_ADDR = 0;
        usb_endp_init();
        USBFSD->INT_FG = USBFS_UIF_BUS_RST;
    } else if (intflag & USBFS_UIF_SUSPEND) {
        USBFSD->INT_FG = USBFS_UIF_SUSPEND;
    } else {
        USBFSD->INT_FG = intflag;
    }
}

/* ============================ entry ============================ */

void dfu_run(void)
{
    dl_addr   = DL_BASE;
    page_len  = 0;
    dfu_state = ST_dfuIDLE;

    usb_init();

    while (1) {
        /* Reboot once the host has read the post-manifest status (so dfu-util
         * finishes cleanly). A calibration-independent busy wait is used on
         * purpose: USB interrupts keep being serviced during it, so the host's
         * final GETSTATUS still completes before we reset. */
        if (reboot_armed) {
            for (volatile uint32_t i = 0; i < DFU_MANIFEST_REBOOT_LOOPS; i++) {
            }
            NVIC_SystemReset();
        }
    }
}
