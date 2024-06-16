#ifndef _DGREADLN_H
#define _DGREADLN_H

typedef uint8_t (*cmd_func_t)(void);

typedef struct {
    const char* name;
    cmd_func_t func;
    const char* descr;
} cmd_t;

extern cmd_t cmd_table[];

char* dgreadln(char* buffer, int mnt, char* prom);
void savehist();

#endif
