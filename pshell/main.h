#ifndef _MAIN_H_
#define _MAIN_H_

#define PSHELL_VERSION "1.2.27 beta"

#include <stdbool.h>
#include <stdint.h>

typedef char buf_t[128];

// Shell global state:
extern int sh_argc;
extern char* sh_argv[];
extern buf_t sh_pwd;

// Terminal global state:
extern uint32_t term_cols;
extern uint32_t term_rows;

// Filesystem global state:
extern bool mounted;

// Temporaries, shared state between functions:
extern buf_t sh_message;
extern buf_t sh_cmd_buffer;
extern buf_t path_tmp;

// Util functions:
char* full_path(const char* name);
void set_translate_crlf(bool enable);
bool bad_mount(bool need);
bool bad_name(void);
bool prompt_Yn(char* prompt);
bool prompt_yN(char* prompt);

#endif
