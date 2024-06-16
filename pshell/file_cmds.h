#ifndef _FILE_CMDS_H
#define _FILE_CMDS_H

#include <stdint.h>

uint8_t mv_cmd(void);
uint8_t cp_cmd(void);
uint8_t mkdir_cmd(void);
uint8_t rm_cmd(void);

uint8_t ls_cmd(void);
uint8_t cat_cmd(void);

#endif
