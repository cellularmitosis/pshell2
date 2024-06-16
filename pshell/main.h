#ifndef _MAIN_H_
#define _MAIN_H_

#include <stdbool.h>

extern int argc;
extern char* argv[];

bool bad_mount(bool need);
bool bad_name(void);

#endif
