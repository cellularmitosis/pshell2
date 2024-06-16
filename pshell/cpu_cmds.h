#ifndef _CPU_CMDS_H
#define _CPU_CMDS_H

#include <stdint.h>

uint8_t reboot_cmd(void);

#if LIB_PICO_STDIO_USB
uint8_t usbboot_cmd(void);
#endif

#endif
