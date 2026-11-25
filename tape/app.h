/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Seth Graves; built on fm1-base by Charles Vestal */
/* TAPE (working name): the probe build. This header is the app's half of the contract with the platform
 * (device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "TAPE PROBE"
#define APP_SPLASH_RGB RGB(214, 160, 90)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */

enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u}

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
