#ifndef _FS_CMDS_H
#define _FS_CMDS_H

#include <stdint.h>

uint8_t format_cmd(void);
uint8_t mount_cmd(void);
uint8_t unmount_cmd(void);
uint8_t status_cmd(void);

#endif
