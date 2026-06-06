/*
 * Copyright (c) 2026 Krzysztof Gawryś
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Example application for the CH32X035 DFU bootloader.
 *
 * Linked at APP_BASE (ld/app.ld). Blinks PA0 so a DFU flash is visibly
 * verifiable. Shows how an application asks the bootloader to enter DFU.
 *
 * Build/flash: see README.md. To re-enter DFU you can:
 *   - call request_dfu() from your app (e.g. on a command/button), or
 *   - hold the DFU button at reset (enable DFU_BUTTON_* in config.h), or
 *   - erase the app region (the bootloader falls back to DFU).
 */
#include "debug.h"
#include "config.h"

/* Reboot into the bootloader's USB DFU mode. */
void request_dfu(void)
{
    *(volatile uint32_t *)BOOT_FLAG_ADDR = BOOT_MAGIC_DFU;
    NVIC_SystemReset();
}

int main(void)
{
    GPIO_InitTypeDef gi = {0};

    SystemCoreClockUpdate();
    Delay_Init();

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gi.GPIO_Pin   = GPIO_Pin_0;
    gi.GPIO_Mode  = GPIO_Mode_Out_PP;
    gi.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gi);

    while (1) {
        GPIO_WriteBit(GPIOA, GPIO_Pin_0, Bit_SET);
        Delay_Ms(200);
        GPIO_WriteBit(GPIOA, GPIO_Pin_0, Bit_RESET);
        Delay_Ms(200);
    }
}
