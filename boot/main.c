/*
 * Copyright (c) 2026 Krzysztof Gawryś
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * CH32X035 DFU bootloader - entry decision.
 *
 * Runs first at reset (flash BOOT_BASE). Decides between USB DFU mode and
 * starting the application at APP_BASE. Stays in Machine mode throughout
 * (see startup_boot.S) so the app's machine-mode startup runs correctly.
 *
 * Enter DFU when any of:
 *   - the application set BOOT_MAGIC_DFU in the RAM flag and reset, or
 *   - (optional) the DFU button is held at reset, or
 *   - (optional) the application region is empty/invalid (anti-brick).
 */
#include "ch32x035.h"
#include "config.h"

void dfu_run(void);   /* dfu.c */

static int button_pressed(void)
{
#if DFU_BUTTON_ENABLE
    GPIO_InitTypeDef gi = {0};
    RCC_APB2PeriphClockCmd(DFU_BUTTON_RCC, ENABLE);
    gi.GPIO_Pin  = DFU_BUTTON_PIN;
    gi.GPIO_Mode = DFU_BUTTON_ACTIVE_LOW ? GPIO_Mode_IPU : GPIO_Mode_IPD;
    GPIO_Init(DFU_BUTTON_PORT, &gi);
    /* small settle */
    for (volatile int i = 0; i < 10000; i++) {
    }
    int level = GPIO_ReadInputDataBit(DFU_BUTTON_PORT, DFU_BUTTON_PIN);
    return DFU_BUTTON_ACTIVE_LOW ? (level == 0) : (level != 0);
#else
    return 0;
#endif
}

int main(void)
{
    volatile uint32_t *flag = (volatile uint32_t *)BOOT_FLAG_ADDR;

    int want_dfu = (*flag == BOOT_MAGIC_DFU);
    *flag = 0;                                  /* consume the request */

    if (button_pressed())
        want_dfu = 1;

    uint32_t first = *(volatile uint32_t *)APP_BASE;
    int app_valid = (first != 0xFFFFFFFFu && first != 0x00000000u);

#if DFU_ENTER_IF_NO_APP
    if (!app_valid)
        want_dfu = 1;
#endif

    if (!want_dfu && app_valid) {
        void (*app_entry)(void) = (void (*)(void))APP_BASE;
        app_entry();
    }

    dfu_run();          /* never returns (reboots after a successful download) */
    while (1) {
    }
}
