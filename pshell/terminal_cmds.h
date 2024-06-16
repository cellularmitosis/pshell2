#ifndef _TERMINAL_CMDS_H
#define _TERMINAL_CMDS_H

#include <stdint.h>
#include <stdbool.h>

uint8_t resize_cmd(void);
uint8_t clear_cmd(void);

bool screen_size(void);

#endif
