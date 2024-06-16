#ifndef _MAIN_H_
#define _MAIN_H_

#include <stdbool.h>

typedef char buf_t[128];

// Shell global state:
extern int sh_argc;
extern char* sh_argv[];
extern buf_t sh_message;

// Filesystem global state:
extern bool mounted;

// Util functions:
char* full_path(const char* name);
void set_translate_crlf(bool enable);
bool bad_mount(bool need);
bool bad_name(void);
bool prompt_Yn(char* prompt);
bool prompt_yN(char* prompt);

#endif
