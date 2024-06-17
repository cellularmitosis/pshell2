#ifndef _CC_WRAPS_H_
#define _CC_WRAPS_H_

// user function shims
void* wrap_malloc(int len);
void* wrap_calloc(int nmemb, int siz);
int wrap_open(char* name, int mode);
int wrap_opendir(char* name);
void wrap_close(int handle);
int wrap_read(int handle, void* buf, int len);
int wrap_readdir(int handle, void* buf);
int wrap_write(int handle, void* buf, int len);
int wrap_lseek(int handle, int pos, int set);
int wrap_popcount(int n);
int wrap_printf(void);
int wrap_sprintf(void);
int wrap_remove(char* name);
int wrap_rename(char* old, char* new);
int wrap_screen_height(void);
int wrap_screen_width(void);
void wrap_wfi(void);

// More shims
char* x_strdup(char* s);
int x_printf(int etype);
int x_sprintf(int etype);

// shim for printf and sprintf (defined in cc_printf.S)
extern int cc_printf(void* stk, int wrds, int prnt);

// accellerated SDK floating point functions
extern void __wrap___aeabi_idiv();
extern void __wrap___aeabi_i2f();
extern void __wrap___aeabi_f2iz();
extern void __wrap___aeabi_fadd();
extern void __wrap___aeabi_fsub();
extern void __wrap___aeabi_fmul();
extern void __wrap___aeabi_fdiv();
extern void __wrap___aeabi_fcmple();
extern void __wrap___aeabi_fcmpgt();
extern void __wrap___aeabi_fcmplt();
extern void __wrap___aeabi_fcmpge();

// accellerate SDK trig functions
extern void __wrap_sinf();
extern void __wrap_cosf();
extern void __wrap_tanf();
extern void __wrap_asinf();
extern void __wrap_acosf();
extern void __wrap_atanf();
extern void __wrap_sinhf();
extern void __wrap_coshf();
extern void __wrap_tanhf();
extern void __wrap_asinhf();
extern void __wrap_acoshf();
extern void __wrap_atanhf();

#endif
