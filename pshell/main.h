#ifndef _MAIN_H_
#define _MAIN_H_

#include <stdbool.h>

typedef char buf_t[128];

extern int sh_argc;
extern char* sh_argv[];
extern buf_t sh_message;

char* full_path(const char* name);

bool bad_mount(bool need);
bool bad_name(void);

#endif