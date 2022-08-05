/*
 * mc is capable of compiling a (subset of) C source files
 * There is no preprocessor.
 *
 * The following options are supported:
 *   -s : Print source and generated representation.
 *
 * All modifications as of Feb 19 2022 are by HPCguy.
 * See AMaCC project repository for baseline code prior to that date.
 *
 * Further modifications by lurk101 for RP Pico
 */

#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <hardware/adc.h>
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/i2c.h>
#include <hardware/irq.h>
#include <hardware/pwm.h>
#include <hardware/spi.h>
#include <hardware/sync.h>

#include <pico/stdio.h>
#include <pico/time.h>

#include "armdisasm.h"
#include "cc.h"
#include "fs.h"

#ifdef NDEBUG
#define Inline inline
#else
#define Inline
#endif

#define K 1024

#define DATA_BYTES (16 * K)
#define TEXT_BYTES (48 * K)
#define SYM_TBL_BYTES (16 * K)
#define TS_TBL_BYTES (2 * K)
#define AST_TBL_BYTES (16 * K)
#define MEMBER_DICT_BYTES (4 * K)
#define STACK_BYTES (16 * K)

#define CTLC 3
#define VT_BOLD "\033[1m"
#define VT_NORMAL "\033[m"

#define ADJ_BITS 5
#define ADJ_MASK ((1 << ADJ_BITS) - 1)

#if PICO_SDK_VERSION_MAJOR > 1 || (PICO_SDK_VERSION_MAJOR == 1 && PICO_SDK_VERSION_MINOR >= 4)
#define SDK14 1
#else
#define SDK14 0
#endif

extern char* full_path(char* name);
extern int cc_printf(void* stk, int wrds, int sflag);
extern void get_screen_xy(int* x, int* y);

extern void __wrap___aeabi_idiv();
extern void __wrap___aeabi_i2f();
extern void __wrap___aeabi_fadd();
extern void __wrap___aeabi_fsub();
extern void __wrap___aeabi_fmul();
extern void __wrap___aeabi_fdiv();
extern void __wrap___aeabi_fcmple();
extern void __wrap___aeabi_fcmpgt();
extern void __wrap___aeabi_fcmplt();
extern void __wrap___aeabi_fcmpge();

extern void test_func();

enum {
    pc_relative = 0,
};

struct patch_s {
    struct patch_s* next;
    uint16_t* addr;
    int val;
    int patch_type;
};

static char *p, *lp;            // current position in source code
static char* data;              // data/bss pointer
static char* data_base;         // data/bss pointer
static char* src;               //
static int* base_sp;            // stack
static uint16_t *e, *le, *text_base; // current position in emitted code
static int* cas;                // case statement patch-up pointer
static struct patch_s* lit;     // literal patch-up pointer
static struct patch_s* def;     // default statement patch-up pointer
static struct patch_s* brks;    // break statement patch-up pointer
static struct patch_s* cnts;    // continue statement patch-up pointer
static int swtc;                // !0 -> in a switch-stmt context
static int brkc;                // !0 -> in a break-stmt context
static int cntc;                // !0 -> in a continue-stmt context
static int* tsize;              // array (indexed by type) of type sizes
static int tnew;                // next available type
static int tk;                  // current token
static union conv {             //
    int i;                      //
    float f;                    //
} tkv;                          // current token value
static int ty;                  // current expression type
                                // bit 0:1 - tensor rank, eg a[4][4][4]
                                // 0=scalar, 1=1d, 2=2d, 3=3d
                                //   1d etype -- bit 0:30)
                                //   2d etype -- bit 0:15,16:30 [32768,65536]
                                //   3d etype -- bit 0:10,11:20,21:30 [1024,1024,2048]
                                // bit 2:9 - type
                                // bit 10:11 - ptr level
static int rtf, rtt;            // return flag and return type for current function
static int loc;                 // local variable offset
static int line;                // current line number
static int src_opt;             // print source and assembly flag
static int trc_opt;             // Trace instruction.
static int* n;                  // current position in emitted abstract syntax tree
                                // With an AST, the compiler is not limited to generate
                                // code on the fly with parsing.
                                // This capability allows function parameter code to be
                                // emitted and pushed on the stack in the proper
                                // right-to-left order.
static int ld;                  // local variable depth
static int pplev, pplevt;       // preprocessor conditional level
static int* ast;                // abstract syntax tree
static ARMSTATE state;          // disassembler state

// identifier
#define MAX_IR 64 // maximum number of local variable or function parameters

struct ident_s {
    int tk; // type-id or keyword
    int hash;
    char* name; // name of this identifier
    /* fields starting with 'h' were designed to save and restore
     * the global class/type/val in order to handle the case if a
     * function declares a local with the same name as a global.
     */
    int class, hclass; // FUNC, GLO (global var), LOC (local var), Syscall
    int type, htype;   // data type such as char and int
    int val, hval;
    int etype, hetype; // extended type info. different meaning for funcs.
    uint16_t* forward; // forward call patch address
};

struct ident_s *id,  // currently parsed identifier
    *sym,            // symbol table (simple list of identifiers)
    *ir_var[MAX_IR]; // IR information for local vars and parameters

static int ir_count;

struct member_s {
    struct member_s* next;
    struct ident_s* id;
    int offset;
    int type;
    int etype;
};

static struct member_s** members; // array (indexed by type) of struct member lists

// tokens and classes (operators last and in precedence order)
// ( >= 128 so not to collide with ASCII-valued tokens)
enum {
#include "cc_tokns.h"
};

// opcodes
/* The instruction set is designed for building intermediate representation.
 * Expression 10 + 20 will be translated into the following instructions:
 *    i = 0;
 *    text[i++] = IMM;
 *    text[i++] = 10;
 *    text[i++] = PSH;
 *    text[i++] = IMM;
 *    text[i++] = 20;
 *    text[i++] = ADD;
 *    text[i++] = PSH;
 *    text[i++] = EXIT;
 *    pc = text;
 */
enum {
    LEA, /*  0 */
/* LEA addressed the problem how to fetch arguments inside sub-function.
 * Let's check out what a calling frame looks like before learning how
 * to fetch arguments (Note that arguments are pushed in its calling
 * order):
 *
 *     sub_function(arg1, arg2, arg3);
 *
 *     |    ....       | high address
 *     +---------------+
 *     | arg: 1        |    new_bp + 4
 *     +---------------+
 *     | arg: 2        |    new_bp + 3
 *     +---------------+
 *     | arg: 3        |    new_bp + 2
 *     +---------------+
 *     |return address |    new_bp + 1
 *     +---------------+
 *     | old BP        | <- new BP
 *     +---------------+
 *     | local var 1   |    new_bp - 1
 *     +---------------+
 *     | local var 2   |    new_bp - 2
 *     +---------------+
 *     |    ....       |  low address
 *
 * If we need to refer to arg1, we need to fetch new_bp + 4, which can not
 * be achieved by restricted ADD instruction. Thus another special
 * instrcution is introduced to do this: LEA <offset>.
 * Together with JSR, ENT, ADJ, LEV, and LEA instruction, we are able to
 * make function calls.
 */
#include "cc_ops.h"
};

static const char* instr_str[] = {
    "LEA", "IMM",  "IMMF", "JMP", "JSR",  "BZ",   "BNZ",  "ENT",  "ADJ",    "LEV",
    "PSH", "PSHF", "LC",   "LI",  "LF",   "SC",   "SI",   "SF",   "OR",     "XOR",
    "AND", "EQ",   "NE",   "GE",  "LT",   "GT",   "LE",   "SHL",  "SHR",    "ADD",
    "SUB", "MUL",  "DIV",  "MOD", "ADDF", "SUBF", "MULF", "DIVF", "FTOI",   "ITOF",
    "EQF", "NEF",  "GEF",  "LTF", "GTF",  "LEF",  "SYSC", "EXIT", "INVALID"};

// types -- 4 scalar types, 1020 aggregate types, 4 tensor ranks, 8 ptr levels
// bits 0-1 = tensor rank, 2-11 = type id, 12-14 = ptr level
// 4 type ids are scalars: 0 = char/void, 1 = int, 2 = float, 3 = reserved
enum { CHAR = 0, INT = 4, FLOAT = 8, ATOM_TYPE = 11, PTR = 0x1000, PTR2 = 0x2000 };

// (library) external functions

struct define_grp {
    char* name;
    int val;
};

static struct define_grp stdio_defines[] = {
    // OPEN
    {"TRUE", 1},
    {"true", 1},
    {"FALSE", 0},
    {"false", 0},
    {"O_RDONLY", LFS_O_RDONLY},
    {"O_WRONLY", LFS_O_WRONLY},
    {"O_RDWR", LFS_O_RDWR},
    {"O_CREAT", LFS_O_CREAT},   // Create a file if it does not exist
    {"O_EXCL", LFS_O_EXCL},     // Fail if a file already exists
    {"O_TRUNC", LFS_O_TRUNC},   // Truncate the existing file to zero size
    {"O_APPEND", LFS_O_APPEND}, // Move to end of file on every write
    {"SEEK_SET", LFS_SEEK_SET}, //
    {"SEEK_CUR", LFS_SEEK_CUR}, //
    {"SEEK_END", LFS_SEEK_END}, //
    {0}};

static struct define_grp gpio_defines[] = {
    // GPIO
    {"GPIO_FUNC_XIP", GPIO_FUNC_XIP},
    {"GPIO_FUNC_SPI", GPIO_FUNC_SPI},
    {"GPIO_FUNC_UART", GPIO_FUNC_UART},
    {"GPIO_FUNC_I2C", GPIO_FUNC_I2C},
    {"GPIO_FUNC_PWM", GPIO_FUNC_PWM},
    {"GPIO_FUNC_SIO", GPIO_FUNC_SIO},
    {"GPIO_FUNC_PIO0", GPIO_FUNC_PIO0},
    {"GPIO_FUNC_PIO1", GPIO_FUNC_PIO1},
    {"GPIO_FUNC_GPCK", GPIO_FUNC_GPCK},
    {"GPIO_FUNC_USB", GPIO_FUNC_USB},
    {"GPIO_FUNC_NULL", GPIO_FUNC_NULL},
    {"GPIO_OUT", GPIO_OUT},
    {"GPIO_IN", GPIO_IN},
    {"GPIO_IRQ_LEVEL_LOW", GPIO_IRQ_LEVEL_LOW},
    {"GPIO_IRQ_LEVEL_HIGH", GPIO_IRQ_LEVEL_HIGH},
    {"GPIO_IRQ_EDGE_FALL", GPIO_IRQ_EDGE_FALL},
    {"GPIO_IRQ_EDGE_RISE", GPIO_IRQ_EDGE_RISE},
    {"GPIO_OVERRIDE_NORMAL", GPIO_OVERRIDE_NORMAL},
    {"GPIO_OVERRIDE_INVERT", GPIO_OVERRIDE_INVERT},
    {"GPIO_OVERRIDE_LOW", GPIO_OVERRIDE_LOW},
    {"GPIO_OVERRIDE_HIGH", GPIO_OVERRIDE_HIGH},
    {"GPIO_SLEW_RATE_SLOW", GPIO_SLEW_RATE_SLOW},
    {"GPIO_SLEW_RATE_FAST", GPIO_SLEW_RATE_FAST},
    {"GPIO_DRIVE_STRENGTH_2MA", GPIO_DRIVE_STRENGTH_2MA},
    {"GPIO_DRIVE_STRENGTH_4MA", GPIO_DRIVE_STRENGTH_4MA},
    {"GPIO_DRIVE_STRENGTH_8MA", GPIO_DRIVE_STRENGTH_8MA},
    {"GPIO_DRIVE_STRENGTH_12MA", GPIO_DRIVE_STRENGTH_12MA},
    // LED
    {"PICO_DEFAULT_LED_PIN", PICO_DEFAULT_LED_PIN},
    {0}};

static struct define_grp pwm_defines[] = {
    // PWM
    {"PWM_DIV_FREE_RUNNING", PWM_DIV_FREE_RUNNING},
    {"PWM_DIV_B_HIGH", PWM_DIV_B_HIGH},
    {"PWM_DIV_B_RISING", PWM_DIV_B_RISING},
    {"PWM_DIV_B_FALLING", PWM_DIV_B_FALLING},
    {"PWM_CHAN_A", PWM_CHAN_A},
    {"PWM_CHAN_B", PWM_CHAN_B},
    {0}};

static struct define_grp clk_defines[] = {
    // CLOCKS
    {"KHZ", KHZ},
    {"MHZ", MHZ},
    {"clk_gpout0", clk_gpout0},
    {"clk_gpout1", clk_gpout1},
    {"clk_gpout2", clk_gpout2},
    {"clk_gpout3", clk_gpout3},
    {"clk_ref", clk_ref},
    {"clk_sys", clk_sys},
    {"clk_peri", clk_peri},
    {"clk_usb", clk_usb},
    {"clk_adc", clk_adc},
    {"clk_rtc", clk_rtc},
    {"CLK_COUNT", CLK_COUNT},
    {0}};

static struct define_grp i2c_defines[] = {
    // I2C
    {"i2c0", (int)&i2c0_inst},
    {"i2c1", (int)&i2c1_inst},
    {"i2c_default", (int)PICO_DEFAULT_I2C_INSTANCE},
    {0}};

static struct define_grp spi_defines[] = {
    // SPI
    {"spi0", (int)spi0_hw},
    {"spi1", (int)spi1_hw},
    {"spi_default", (int)PICO_DEFAULT_SPI_INSTANCE},
    {0}};

static struct define_grp math_defines[] = {{0}};

static struct define_grp adc_defines[] = {{0}};

static struct define_grp stdlib_defines[] = {{0}};

static struct define_grp string_defines[] = {{0}};

static struct define_grp time_defines[] = {{0}};

static struct define_grp sync_defines[] = {{0}};

static struct define_grp irq_defines[] = {
    // IRQ
    {"TIMER_IRQ_0", TIMER_IRQ_0},
    {"TIMER_IRQ_1", TIMER_IRQ_1},
    {"TIMER_IRQ_2", TIMER_IRQ_2},
    {"TIMER_IRQ_3", TIMER_IRQ_3},
    {"PWM_IRQ_WRAP", PWM_IRQ_WRAP},
    {"USBCTRL_IRQ", USBCTRL_IRQ},
    {"XIP_IRQ", XIP_IRQ},
    {"PIO0_IRQ_0", PIO0_IRQ_0},
    {"PIO0_IRQ_1", PIO0_IRQ_1},
    {"PIO1_IRQ_0", PIO1_IRQ_0},
    {"PIO1_IRQ_1", PIO1_IRQ_1},
    {"DMA_IRQ_0", DMA_IRQ_0},
    {"DMA_IRQ_1", DMA_IRQ_1},
    {"IO_IRQ_BANK0", IO_IRQ_BANK0},
    {"IO_IRQ_QSPI", IO_IRQ_QSPI},
    {"SIO_IRQ_PROC0", SIO_IRQ_PROC0},
    {"SIO_IRQ_PROC1", SIO_IRQ_PROC1},
    {"CLOCKS_IRQ", CLOCKS_IRQ},
    {"SPI0_IRQ", SPI0_IRQ},
    {"SPI1_IRQ", SPI1_IRQ},
    {"UART0_IRQ", UART0_IRQ},
    {"UART1_IRQ", UART1_IRQ},
    {"ADC_IRQ_FIFO", ADC_IRQ_FIFO},
    {"I2C0_IRQ", I2C0_IRQ},
    {"I2C1_IRQ", I2C1_IRQ},
    {"RTC_IRQ", RTC_IRQ},
    {"PICO_DEFAULT_IRQ_PRIORITY", PICO_DEFAULT_IRQ_PRIORITY},
    {"PICO_LOWEST_IRQ_PRIORITY", PICO_LOWEST_IRQ_PRIORITY},
    {"PICO_HIGHEST_IRQ_PRIORITY", PICO_HIGHEST_IRQ_PRIORITY},
    {"PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY",
     PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY},
    {"PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY",
     PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY},
    {"PICO_SHARED_IRQ_HANDLER_LOWEST_ORDER_PRIORITY",
     PICO_SHARED_IRQ_HANDLER_LOWEST_ORDER_PRIORITY},
    {0}};

static jmp_buf done_jmp;
static int* malloc_list;

#define fatal(fmt, ...) fatal_func(__FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)

static __attribute__((__noreturn__)) void fatal_func(const char* func, int lne, const char* fmt,
                                                     ...) {
    printf("\n");
#ifndef NDEBUG
    printf("error in compiler function %s at line %d\n", func, lne);
#endif
    printf(VT_BOLD "Error : " VT_NORMAL);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (line > 0) {
        lp = src;
        lne = line;
        while (--lne)
            lp = strchr(lp, '\n') + 1;
        p = strchr(lp, '\n');
        printf("\n" VT_BOLD "%d:" VT_NORMAL " %.*s", line, p - lp, lp);
    }
    printf("\n");
    longjmp(done_jmp, 1);
}

static __attribute__((__noreturn__)) void run_fatal(const char* fmt, ...) {
    printf("\n" VT_BOLD "run time error : " VT_NORMAL);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    longjmp(done_jmp, 1);
}

static void* sys_malloc(int l, int die) {
    int* p = malloc(l + 8);
    if (!p) {
        if (die)
            fatal("out of memory");
        else
            return 0;
    }
    memset(p + 2, 0, l);
    p[0] = (int)malloc_list;
    malloc_list = p;
    return p + 2;
}

static void sys_free(void* p) {
    if (!p)
        fatal("freeing a NULL pointer");
    int* p2 = (int*)p - 2;
    int* last = (int*)&malloc_list;
    int* pi = (int*)(*last);
    while (pi) {
        if (pi == p2) {
            last[0] = pi[0];
            free(pi);
            return;
        }
        last = pi;
        pi = (int*)pi[0];
    }
    fatal("corrupted memory");
}

// stubs for now
static void* wrap_malloc(int len) { return sys_malloc(len, 0); };

static struct file_handle {
    struct file_handle* next;
    bool is_dir;
    union {
        lfs_file_t file;
        lfs_dir_t dir;
    } u;
} * file_list;

static int wrap_open(char* name, int mode) {
    struct file_handle* h = sys_malloc(sizeof(struct file_handle), 1);
    h->is_dir = false;
    if (fs_file_open(&h->u.file, full_path(name), mode) < LFS_ERR_OK) {
        sys_free(h);
        return 0;
    }
    h->next = file_list;
    file_list = h;
    return (int)h;
}

static int wrap_opendir(char* name) {
    struct file_handle* h = sys_malloc(sizeof(struct file_handle), 1);
    h->is_dir = true;
    if (fs_dir_open(&h->u.dir, full_path(name)) < LFS_ERR_OK) {
        sys_free(h);
        return 0;
    }
    h->next = file_list;
    file_list = h;
    return (int)h;
}

static void wrap_close(int handle) {
    struct file_handle* last_h = (void*)&file_list;
    struct file_handle* h = file_list;
    while (h) {
        if (h == (struct file_handle*)handle) {
            last_h->next = h->next;
            if (h->is_dir)
                fs_dir_close(&h->u.dir);
            else
                fs_file_close(&h->u.file);
            sys_free(h);
            return;
        }
        last_h = h;
        h = h->next;
    }
    fatal("closing unopened file!");
}

static int wrap_read(int handle, void* buf, int len) {
    struct file_handle* h = (struct file_handle*)handle;
    if (h->is_dir)
        fatal("use readdir to read from directories");
    return fs_file_read(&h->u.file, buf, len);
}

static int wrap_readdir(int handle, void* buf) {
    struct file_handle* h = (struct file_handle*)handle;
    if (!h->is_dir)
        fatal("use read to read from files");
    return fs_dir_read(&h->u.dir, buf);
}

static int wrap_write(int handle, void* buf, int len) {
    struct file_handle* h = (struct file_handle*)handle;
    return fs_file_write(&h->u.file, buf, len);
}

static int wrap_lseek(int handle, int pos, int set) {
    struct file_handle* h = (struct file_handle*)handle;
    return fs_file_seek(&h->u.file, pos, set);
};

static int wrap_popcount(int n) { return __builtin_popcount(n); };

static int wrap_printf(void){};
static int wrap_sprintf(void){};

static int wrap_remove(char* name) { return fs_remove(full_path(name)); };

static int wrap_rename(char* old, char* new) {
    char* fp = full_path(old);
    char* fpa = sys_malloc(strlen(fp) + 1, 1);
    strcpy(fpa, fp);
    char* fpb = full_path(new);
    int r = fs_rename(fpa, fpb);
    sys_free(fpa);
    return r;
}

static int wrap_screen_height(void) {
    int x, y;
    get_screen_xy(&x, &y);
    return y;
}

static int wrap_screen_width(void) {
    int x, y;
    get_screen_xy(&x, &y);
    return x;
}

static void wrap_wfi(void) { __wfi(); };

static float wrap_aeabi_fadd(float a, float b) { return a + b; }

static float wrap_aeabi_fdiv(float a, float b) { return a / b; }

static float wrap_aeabi_fmul(float a, float b) { return a * b; }

static float wrap_aeabi_fsub(float a, float b) { return a - b; }

static const struct {
    char* name;
    int etype;
    struct define_grp* grp;
    void* extrn;
    int ret_float : 1;
    int is_printf : 1;
    int is_sprintf : 1;
} externs[] = {
#include "cc_extrns.h"
};

static struct {
    char* name;
    struct define_grp* grp;
} includes[] = {{"stdio", stdio_defines},   {"stdlib", stdlib_defines},
                {"string", string_defines}, {"math", math_defines},
                {"sync", sync_defines},     {"time", time_defines},
                {"gpio", gpio_defines},     {"pwm", pwm_defines},
                {"adc", adc_defines},       {"clocks", clk_defines},
                {"i2c", i2c_defines},       {"spi", spi_defines},
                {"irq", irq_defines},       {0}};

static lfs_file_t* fd;
static char* fp;

static void clear_globals(void) {
    e = le = text_base = NULL;
    base_sp = cas = tsize = n = malloc_list =
        (int*)(data_base = data = src = p = lp = fp = (char*)(id = sym = NULL));
    lit = def = brks = cnts = NULL;
    fd = NULL;
    file_list = NULL;
    swtc = brkc = cntc = tnew = tk = ty = loc = line = src_opt = trc_opt = ld = pplev = pplevt =
        ir_count = 0;
    memset(&tkv, 0, sizeof(tkv));
    memset(ir_var, 0, sizeof(ir_var));
    memset(&members, 0, sizeof(members));
    memset(&done_jmp, 0, sizeof(&done_jmp));
}

#define numof(a) (sizeof(a) / sizeof(a[0]))

static int extern_search(char* name) // get cache index of external function
{
    int first = 0, last = numof(externs) - 1, middle;
    while (first <= last) {
        middle = (first + last) / 2;
        if (strcmp(name, externs[middle].name) > 0)
            first = middle + 1;
        else if (strcmp(name, externs[middle].name) < 0)
            last = middle - 1;
        else
            return middle;
    }
    return -1;
}

/* parse next token
 * 1. store data into id and then set the id to current lexcial form
 * 2. set tk to appropriate type
 */
static void next() {
    char* pp;
    int t;

    /* using loop to ignore whitespace characters, but characters that
     * cannot be recognized by the lexical analyzer are considered blank
     * characters, such as '@' and '$'.
     */
    while ((tk = *p)) {
        ++p;
        if ((tk >= 'a' && tk <= 'z') || (tk >= 'A' && tk <= 'Z') || (tk == '_')) {
            pp = p - 1;
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || (*p == '_'))
                tk = tk * 147 + *p++;
            tk = (tk << 6) + (p - pp); // hash plus symbol length
            // hash value is used for fast comparison. Since it is inaccurate,
            // we have to validate the memory content as well.
            for (id = sym; id->tk; ++id) { // find one free slot in table
                if (tk == id->hash &&      // if token is found (hash match), overwrite
                    !memcmp(id->name, pp, p - pp)) {
                    tk = id->tk;
                    return;
                }
            }
            /* At this point, existing symbol name is not found.
             * "id" points to the first unused symbol table entry.
             */
            id->name = pp;
            id->hash = tk;
            id->forward = 0;
            tk = id->tk = Id; // token type identifier
            return;
        }
        /* Calculate the constant */
        // first byte is a number, and it is considered a numerical value
        else if (tk >= '0' && tk <= '9') {
            tk = Num;                             // token is char or int
            tkv.i = strtoul((pp = p - 1), &p, 0); // octal, decimal, hex parsing
            if (*p == '.') {
                tkv.f = strtof(pp, &p);
                tk = NumF;
            } // float
            return;
        }
        switch (tk) {
        case '\n':
            if (src_opt) {
                printf("%d: %.*s", line, p - lp, lp);
            }
            lp = p;
            ++line;
        case ' ':
        case '\t':
        case '\v':
        case '\f':
        case '\r':
            break;
        case '/':
            if (*p == '/') { // comment
                while (*p != 0 && *p != '\n')
                    ++p;
            } else if (*p == '*') { // C-style multiline comments
                t = 0;
                for (++p; (*p != 0) && (t == 0); ++p) {
                    pp = p + 1;
                    if (*p == '\n')
                        ++line;
                    else if (*p == '*' && *pp == '/')
                        t = 1;
                }
                ++p;
            } else {
                if (*p == '=') {
                    ++p;
                    tk = DivAssign;
                } else
                    tk = Div;
                return;
            }
            break;
        case '#': // skip include statements, and most preprocessor directives
            if (!strncmp(p, "define", 6)) {
                p += 6;
                next();
                if (tk == Id) {
                    next();
                    if (tk == Num) {
                        id->class = Num;
                        id->type = INT;
                        id->val = tkv.i;
                    }
                }
            } else if ((t = !strncmp(p, "ifdef", 5)) || !strncmp(p, "ifndef", 6)) {
                p += 6;
                next();
                if (tk != Id)
                    fatal("No identifier");
                ++pplev;
                if ((((id->class != Num) ? 0 : 1) ^ (t ? 1 : 0)) & 1) {
                    t = pplevt;
                    pplevt = pplev - 1;
                    while (*p != 0 && *p != '\n')
                        ++p; // discard until end-of-line
                    do
                        next();
                    while (pplev != pplevt);
                    pplevt = t;
                }
            } else if (!strncmp(p, "if", 2)) {
                // ignore side effects of preprocessor if-statements
                ++pplev;
            } else if (!strncmp(p, "endif", 5)) {
                if (--pplev < 0)
                    fatal("preprocessor context nesting error");
                if (pplev == pplevt)
                    return;
            }
            while (*p != 0 && *p != '\n')
                ++p; // discard until end-of-line
            break;
        case '\'': // quotes start with character (string)
        case '"':
            pp = data;
            while (*p != 0 && *p != tk) {
                if ((tkv.i = *p++) == '\\') {
                    switch (tkv.i = *p++) {
                    case 'n':
                        tkv.i = '\n';
                        break; // new line
                    case 't':
                        tkv.i = '\t';
                        break; // horizontal tab
                    case 'v':
                        tkv.i = '\v';
                        break; // vertical tab
                    case 'f':
                        tkv.i = '\f';
                        break; // form feed
                    case 'r':
                        tkv.i = '\r';
                        break; // carriage return
                    case '0':
                        tkv.i = '\0';
                        break; // an int with value 0
                    }
                }
                // if it is double quotes (string literal), it is considered as
                // a string, copying characters to data
                if (tk == '"') {
                    if (data >= data_base + (DATA_BYTES / 4))
                        fatal("program data exceeds data segment");
                    *data++ = tkv.i;
                }
            }
            ++p;
            if (tk == '"')
                tkv.i = (int)pp;
            else
                tk = Num;
            return;
        case '=':
            if (*p == '=') {
                ++p;
                tk = Eq;
            } else
                tk = Assign;
            return;
        case '*':
            if (*p == '=') {
                ++p;
                tk = MulAssign;
            } else
                tk = Mul;
            return;
        case '+':
            if (*p == '+') {
                ++p;
                tk = Inc;
            } else if (*p == '=') {
                ++p;
                tk = AddAssign;
            } else
                tk = Add;
            return;
        case '-':
            if (*p == '-') {
                ++p;
                tk = Dec;
            } else if (*p == '>') {
                ++p;
                tk = Arrow;
            } else if (*p == '=') {
                ++p;
                tk = SubAssign;
            } else
                tk = Sub;
            return;
        case '[':
            tk = Bracket;
            return;
        case '&':
            if (*p == '&') {
                ++p;
                tk = Lan;
            } else if (*p == '=') {
                ++p;
                tk = AndAssign;
            } else
                tk = And;
            return;
        case '!':
            if (*p == '=') {
                ++p;
                tk = Ne;
            }
            return;
        case '<':
            if (*p == '=') {
                ++p;
                tk = Le;
            } else if (*p == '<') {
                ++p;
                if (*p == '=') {
                    ++p;
                    tk = ShlAssign;
                } else
                    tk = Shl;
            } else
                tk = Lt;
            return;
        case '>':
            if (*p == '=') {
                ++p;
                tk = Ge;
            } else if (*p == '>') {
                ++p;
                if (*p == '=') {
                    ++p;
                    tk = ShrAssign;
                } else
                    tk = Shr;
            } else
                tk = Gt;
            return;
        case '|':
            if (*p == '|') {
                ++p;
                tk = Lor;
            } else if (*p == '=') {
                ++p;
                tk = OrAssign;
            } else
                tk = Or;
            return;
        case '^':
            if (*p == '=') {
                ++p;
                tk = XorAssign;
            } else
                tk = Xor;
            return;
        case '%':
            if (*p == '=') {
                ++p;
                tk = ModAssign;
            } else
                tk = Mod;
            return;
        case '?':
            tk = Cond;
            return;
        case '.':
            tk = Dot;
        default:
            return;
        }
    }
}

typedef struct {
    int tk;
    int v1;
} Double_entry_t;
#define Double_entry(a) (*((Double_entry_t*)a))

typedef struct {
    int tk;
    int next;
    int addr;
    int n_parms;
    int parm_types;
} Func_entry_t;
#define Func_entry(a) (*((Func_entry_t*)a))

static void ast_Func(int parm_types, int n_parms, int addr, int next, int tk) {
    n -= sizeof(Func_entry_t) / sizeof(int);
    Func_entry(n).parm_types = parm_types;
    Func_entry(n).n_parms = n_parms;
    Func_entry(n).addr = addr;
    Func_entry(n).next = next;
    Func_entry(n).tk = tk;
}

typedef struct {
    int tk;
    int cond;
    int incr;
    int body;
    int init;
} For_entry_t;
#define For_entry(a) (*((For_entry_t*)a))

static void ast_For(int init, int body, int incr, int cond) {
    n -= sizeof(For_entry_t) / sizeof(int);
    For_entry(n).init = init;
    For_entry(n).body = body;
    For_entry(n).incr = incr;
    For_entry(n).cond = cond;
    For_entry(n).tk = For;
}

typedef struct {
    int tk;
    int cond_part;
    int if_part;
    int else_part;
} Cond_entry_t;
#define Cond_entry(a) (*((Cond_entry_t*)a))

static void ast_Cond(int else_part, int if_part, int cond_part) {
    n -= sizeof(Cond_entry_t) / sizeof(int);
    Cond_entry(n).else_part = else_part;
    Cond_entry(n).if_part = if_part;
    Cond_entry(n).cond_part = cond_part;
    Cond_entry(n).tk = Cond;
}

typedef struct {
    int tk;
    int type;
    int right_part;
} Assign_entry_t;
#define Assign_entry(a) (*((Assign_entry_t*)a))

static void ast_Assign(int right_part, int type) {
    n -= sizeof(Assign_entry_t) / sizeof(int);
    Assign_entry(n).right_part = right_part;
    Assign_entry(n).type = type;
    Assign_entry(n).tk = Assign;
}

typedef struct {
    int tk;
    int body;
    int cond;
} While_entry_t;
#define While_entry(a) (*((While_entry_t*)a))

static void ast_While(int cond, int body, int tk) {
    n -= sizeof(While_entry_t) / sizeof(int);
    While_entry(n).cond = cond;
    While_entry(n).body = body;
    While_entry(n).tk = tk;
}

typedef struct {
    int tk;
    int cond;
    int cas;
} Switch_entry_t;
#define Switch_entry(a) (*((Switch_entry_t*)a))

static void ast_Switch(int cas, int cond) {
    n -= sizeof(Switch_entry_t) / sizeof(int);
    Switch_entry(n).cas = cas;
    Switch_entry(n).cond = cond;
    Switch_entry(n).tk = Switch;
}

typedef struct {
    int tk;
    int next;
    int expr;
} Case_entry_t;
#define Case_entry(a) (*((Case_entry_t*)a))

static void ast_Case(int expr, int next) {
    n -= sizeof(Case_entry_t) / sizeof(int);
    Case_entry(n).expr = expr;
    Case_entry(n).next = next;
    Case_entry(n).tk = Case;
}

typedef struct {
    int tk;
    int val;
    int way;
} CastF_entry_t;
#define CastF_entry(a) (*((CastF_entry_t*)a))

static void ast_CastF(int way, int val) {
    n -= sizeof(CastF_entry_t) / sizeof(int);
    CastF_entry(n).tk = CastF;
    CastF_entry(n).val = val;
    CastF_entry(n).way = way;
}

typedef struct {
    int tk;
    int val;
} Enter_entry_t;
#define Enter_entry(a) (*((Enter_entry_t*)a))

static uint16_t* ast_Enter(int val) {
    n -= sizeof(Enter_entry_t) / sizeof(int);
    Enter_entry(n).tk = Enter;
    Enter_entry(n).val = val;
}

// two word entries
static void ast_Return(int v1) {
    n -= sizeof(Double_entry_t) / sizeof(int);
    Double_entry(n).tk = Return;
    Double_entry(n).v1 = v1;
}

typedef struct {
    int tk;
    int oprnd;
} Oper_entry_t;
#define Oper_entry(a) (*((Oper_entry_t*)a))

static void ast_Oper(int oprnd, int op) {
    n -= sizeof(Oper_entry_t) / sizeof(int);
    Oper_entry(n).tk = op;
    Oper_entry(n).oprnd = oprnd;
}

static void ast_Num(int v1) {
    n -= sizeof(Double_entry_t) / sizeof(int);
    Double_entry(n).tk = Num;
    Double_entry(n).v1 = v1;
}

static void ast_Label(int v1) {
    n -= sizeof(Double_entry_t) / sizeof(int);
    Double_entry(n).tk = Label;
    Double_entry(n).v1 = v1;
}

static void ast_Goto(int v1) {
    n -= sizeof(Double_entry_t) / sizeof(int);
    Double_entry(n).tk = Goto;
    Double_entry(n).v1 = v1;
}

static void ast_Default(int v1) {
    n -= sizeof(Double_entry_t) / sizeof(int);
    Double_entry(n).tk = Default;
    Double_entry(n).v1 = v1;
}

static void ast_NumF(int v1) {
    n -= sizeof(Double_entry_t) / sizeof(int);
    Double_entry(n).tk = NumF;
    Double_entry(n).v1 = v1;
}

static void ast_Loc(int v1) {
    n -= sizeof(Double_entry_t) / sizeof(int);
    Double_entry(n).tk = Loc;
    Double_entry(n).v1 = v1;
}

static void ast_Load(int v1) {
    n -= sizeof(Double_entry_t) / sizeof(int);
    Double_entry(n).tk = Load;
    Double_entry(n).v1 = v1;
}

typedef struct {
    int tk;
    int addr;
} Begin_entry_t;
#define Begin_entry(a) (*((Begin_entry_t*)a))

static void ast_Begin(int v1) {
    n -= sizeof(Begin_entry_t) / sizeof(int);
    Begin_entry(n).tk = '{';
    Begin_entry(n).addr = v1;
}

// single word entry

typedef struct {
    int tk;
} Single_entry_t;
#define Single_entry(a) (*((Single_entry_t*)a))

#define ast_Tk(a) (Single_entry(a).tk)
#define ast_NumVal(a) (Double_entry(a).v1)

static void ast_Single(int k) {
    n -= sizeof(Single_entry_t) / sizeof(int);
    Single_entry(n).tk = k;
}

// verify binary operations are legal
static void typecheck(int op, int tl, int tr) {
    int pt = 0, it = 0, st = 0;
    if (tl >= PTR)
        pt += 2; // is pointer?
    if (tr >= PTR)
        pt += 1;

    if (tl < FLOAT)
        it += 2; // is int?
    if (tr < FLOAT)
        it += 1;

    if (tl > ATOM_TYPE && tl < PTR)
        st += 2; // is struct/union?
    if (tr > ATOM_TYPE && tr < PTR)
        st += 1;

    if ((tl ^ tr) & (PTR | PTR2)) { // operation on different pointer levels
        if (op == Add && pt != 3 && (it & ~pt))
            ; // ptr + int or int + ptr ok
        else if (op == Sub && pt == 2 && it == 1)
            ; // ptr - int ok
        else if (op == Assign && pt == 2 && ast_Tk(n) == Num && ast_NumVal(n) == 0)
            ; // ok
        else if (op >= Eq && op <= Le && ast_Tk(n) == Num && ast_NumVal(n) == 0)
            ; // ok
        else
            fatal("bad pointer arithmetic or cast needed");
    } else if (pt == 3 && op != Assign && op != Sub &&
               (op < Eq || op > Le)) // pointers to same type
        fatal("bad pointer arithmetic");

    if (pt == 0 && op != Assign && (it == 1 || it == 2))
        fatal("cast operation needed");

    if (pt == 0 && st != 0)
        fatal("illegal operation with dereferenced struct");
}

static void bitopcheck(int tl, int tr) {
    if (tl >= FLOAT || tr >= FLOAT)
        fatal("bit operation on non-int types");
}

static bool is_power_of_2(int n) { return ((n - 1) & n) == 0; }

/* expression parsing
 * lev represents an operator.
 * because each operator `token` is arranged in order of priority,
 * large `lev` indicates a high priority.
 *
 * Operator precedence (lower first):
 * Assign  =
 * Cond   ?
 * Lor    ||
 * Lan    &&
 * Or     |
 * Xor    ^
 * And    &
 * Eq     ==
 * Ne     !=
 * Ge     >=
 * Lt     <
 * Gt     >
 * Le     <=
 * Shl    <<
 * Shr    >>
 * Add    +
 * Sub    -
 * Mul    *
 * Div    /
 * Mod    %
 * Inc    ++
 * Dec    --
 * Bracket [
 */

static void expr(int lev) {
    int t, tc, tt, nf, *b, sz, *c;
    int otk, memsub = 0;
    struct ident_s* d;
    struct member_s* m;

    switch (tk) {
    case Id:
        d = id;
        next();
        // function call
        if (tk == '(') {
            if (d->class == Func && d->val == 0)
                goto resolve_fnproto;
            if (d->class < Func || d->class > Syscall) {
                if (d->class != 0)
                    fatal("bad function call");
                d->type = INT;
                d->etype = 0;
            resolve_fnproto:
                d->class = Syscall;
                int namelen = d->hash & 0x3f;
                char ch = d->name[namelen];
                d->name[namelen] = 0;
                int ix = extern_search(d->name);
                if (ix < 0)
                    fatal("Unknown external function %s", d->name);
                d->val = ix;
                d->type = externs[ix].ret_float ? FLOAT : INT;
                d->etype = externs[ix].etype;
                d->name[namelen] = ch;
            }
            next();
            t = 0;
            b = c = 0;
            tt = 0;
            nf = 0; // argument count
            while (tk != ')') {
                expr(Assign);
                if (c != 0) {
                    ast_Begin((int)c);
                    c = 0;
                }
                ast_Single((int)b);
                b = n;
                ++t;
                tt = tt * 2;
                if (ty == FLOAT) {
                    ++nf;
                    ++tt;
                }
                if (tk == ',') {
                    next();
                    if (tk == ')')
                        fatal("unexpected comma in function call");
                } else if (tk != ')')
                    fatal("missing comma in function call");
            }
            if (t > ADJ_MASK)
                fatal("maximum of %d function parameters", ADJ_MASK);
            tt = (tt << 10) + (nf << 5) + t; // func etype not like other etype
            if (d->etype != tt && !externs[d->val].is_printf && externs[d->val].is_sprintf)
                fatal("argument type mismatch");
            next();
            // function or system call id
            ast_Func(tt, t, d->val, (int)b, d->class);
            ty = d->type;
        }
        // enumeration, only enums have ->class == Num
        else if ((d->class == Num) || (d->class == Func)) {
            ast_Num(d->val);
            ty = INT;
        } else {
            // Variable get offset
            switch (d->class) {
            case Loc:
            case Par:
                ast_Loc(loc - d->val);
                break;
            case Func:
            case Glo:
                ast_Num(d->val);
                break;
            default:
                fatal("undefined variable %.*s", d->hash & ADJ_MASK, d->name);
            }
            if ((d->type & 3) && d->class != Par) { // push reference address
                ty = d->type & ~3;
            } else {
                ast_Load((ty = d->type & ~3));
            }
        }
        break;
    // directly take an immediate value as the expression value
    // IMM recorded in emit sequence
    case Num:
        ast_Num(tkv.i);
        next();
        ty = INT;
        break;
    case NumF:
        ast_NumF(tkv.i);
        next();
        ty = FLOAT;
        break;
    case '"': // string, as a literal in data segment
        ast_Num(tkv.i);
        next();
        // continuous `"` handles C-style multiline text such as `"abc" "def"`
        while (tk == '"') {
            if (data >= data_base + (DATA_BYTES / 4))
                fatal("program data exceeds data segment");
            next();
        }
        if (data >= data_base + (DATA_BYTES / 4))
            fatal("program data exceeds data segment");
        data = (char*)(((int)data + sizeof(int)) & (-sizeof(int)));
        ty = CHAR + PTR;
        break;
    /* SIZEOF_expr -> 'sizeof' '(' 'TYPE' ')'
     * FIXME: not support "sizeof (Id)".
     */
    case Sizeof:
        next();
        if (tk != '(')
            fatal("open parenthesis expected in sizeof");
        next();
        d = 0;
        if (tk == Num || tk == NumF) {
            ty = (Int - Char) << 2;
            next();
        } else if (tk == Id) {
            d = id;
            ty = d->type;
            next();
        } else {
            ty = INT; // Enum
            switch (tk) {
            case Char:
            case Int:
            case Float:
                ty = (tk - Char) << 2;
                next();
                break;
            case Struct:
            case Union:
                next();
                if (tk != Id || id->type <= ATOM_TYPE || id->type >= PTR)
                    fatal("bad struct/union type");
                ty = id->type;
                next();
                break;
            }
            // multi-level pointers, plus `PTR` for each level
            while (tk == Mul) {
                next();
                ty += PTR;
            }
        }
        if (tk != ')')
            fatal("close parenthesis expected in sizeof");
        next();
        ast_Num((ty & 3) ? (((ty - PTR) >= PTR) ? sizeof(int) : tsize[(ty - PTR) >> 2])
                         : ((ty >= PTR) ? sizeof(int) : tsize[ty >> 2]));
        // just one dimension supported at the moment
        if (d != 0 && (ty & 3))
            ast_NumVal(n) *= (id->etype + 1);
        ty = INT;
        break;
    // Type cast or parenthesis
    case '(':
        next();
        if (tk >= Char && tk <= Union) {
            switch (tk) {
            case Char:
            case Int:
            case Float:
                t = (tk - Char) << 2;
                next();
                break;
            default:
                next();
                if (tk != Id || id->type <= ATOM_TYPE || id->type >= PTR)
                    fatal("bad struct/union type");
                t = id->type;
                next();
                break;
            }
            // t: pointer
            while (tk == Mul) {
                next();
                t += PTR;
            }
            if (tk != ')')
                fatal("bad cast");
            next();
            expr(Inc); // cast has precedence as Inc(++)
            if (t != ty && (t == FLOAT || ty == FLOAT)) {
                if (t == FLOAT && ty < FLOAT) { // float : int
                    if (ast_Tk(n) == Num) {
                        ast_Tk(n) = NumF;
                        *((float*)&ast_NumVal(n)) = ast_NumVal(n);
                    } else {
                        b = n;
                        ast_CastF(ITOF, (int)b);
                    }
                } else if (t < FLOAT && ty == FLOAT) { // int : float
                    if (ast_Tk(n) == NumF) {
                        ast_Tk(n) = Num;
                        ast_NumVal(n) = *((float*)&ast_NumVal(n));
                    } else {
                        b = n;
                        ast_CastF(FTOI, (int)b);
                    }
                } else
                    fatal("explicit cast required");
            }
            ty = t;
        } else {
            expr(Assign);
            while (tk == ',') {
                next();
                b = n;
                expr(Assign);
                if (b != n)
                    ast_Begin((int)b);
            }
            if (tk != ')')
                fatal("close parenthesis expected");
            next();
        }
        break;
    case Mul: // "*", dereferencing the pointer operation
        next();
        expr(Inc); // dereference has the same precedence as Inc(++)
        if (ty < PTR)
            fatal("bad dereference");
        ty -= PTR;
        ast_Load(ty);
        break;
    case And: // "&", take the address operation
        /* when "token" is a variable, it takes the address first and
         * then LI/LC, so `--e` becomes the address of "a".
         */
        next();
        expr(Inc);
        if (ast_Tk(n) != Load)
            fatal("bad address-of");
        n += 2;
        ty += PTR;
        break;
    case '!': // "!x" is equivalent to "x == 0"
        next();
        expr(Inc);
        if (ty > ATOM_TYPE && ty < PTR)
            fatal("!(struct/union) is meaningless");
        if (ast_Tk(n) == Num)
            ast_NumVal(n) = !ast_NumVal(n);
        else {
            ast_Num(0);
            ast_Oper((int)(n + 2), Eq);
        }
        ty = INT;
        break;
    case '~': // "~x" is equivalent to "x ^ -1"
        next();
        expr(Inc);
        if (ty > ATOM_TYPE)
            fatal("~ptr is illegal");
        if (ast_Tk(n) == Num)
            ast_NumVal(n) = ~ast_NumVal(n);
        else {
            ast_Num(-1);
            ast_Oper((int)(n + 2), Xor);
        }
        ty = INT;
        break;
    case Add:
        next();
        expr(Inc);
        if (ty > ATOM_TYPE)
            fatal("unary '+' illegal on ptr");
        break;
    case Sub:
        next();
        expr(Inc);
        if (ty > ATOM_TYPE)
            fatal("unary '-' illegal on ptr");
        if (ast_Tk(n) == Num)
            ast_NumVal(n) = -ast_NumVal(n);
        else if (ast_Tk(n) == NumF) {
            ast_NumVal(n) ^= 0x80000000;
        } else if (ty == FLOAT) {
            ast_NumF(0xbf800000);
            ast_Oper((int)(n + 2), MulF);
        } else {
            ast_Num(-1);
            ast_Oper((int)(n + 2), Mul);
        }
        if (ty != FLOAT)
            ty = INT;
        break;
    case Inc:
    case Dec: // processing ++x and --x. x-- and x++ is handled later
        t = tk;
        next();
        expr(Inc);
        if (ty == FLOAT)
            fatal("no ++/-- on float");
        if (ast_Tk(n) != Load)
            fatal("bad lvalue in pre-increment");
        ast_Tk(n) = t;
        break;
    case 0:
        fatal("unexpected EOF in expression");
    default:
        fatal("bad expression");
    }

    // "precedence climbing" or "Top Down Operator Precedence" method
    while (tk >= lev) {
        // tk is ASCII code will not exceed `Num=128`. Its value may be changed
        // during recursion, so back up currently processed expression type
        t = ty;
        b = n;
        switch (tk) {
        case Assign:
            if (t & 3)
                fatal("Cannot assign to array type lvalue");
            // the left part is processed by the variable part of `tk=ID`
            // and pushes the address
            if (ast_Tk(n) != Load)
                fatal("bad lvalue in assignment");
            // get the value of the right part `expr` as the result of `a=expr`
            n += 2;
            b = n;
            next();
            expr(Assign);
            typecheck(Assign, t, ty);
            ast_Assign((int)b, (ty << 16) | t);
            ty = t;
            break;
        case OrAssign: // right associated
        case XorAssign:
        case AndAssign:
        case ShlAssign:
        case ShrAssign:
        case AddAssign:
        case SubAssign:
        case MulAssign:
        case DivAssign:
        case ModAssign:
            if (t & 3)
                fatal("Cannot assign to array type lvalue");
            if (ast_Tk(n) != Load)
                fatal("bad lvalue in assignment");
            otk = tk;
            n += 2;
            b = n;
            ast_Single(';');
            ast_Load(t);
            sz = (t >= PTR2) ? sizeof(int) : ((t >= PTR) ? tsize[(t - PTR) >> 2] : 1);
            next();
            c = n;
            expr(otk);
            if (ast_Tk(n) == Num)
                ast_NumVal(n) *= sz;
            ast_Oper((int)c, (otk < ShlAssign) ? Or + (otk - OrAssign) : Shl + (otk - ShlAssign));
            if (t == FLOAT && (otk >= AddAssign && otk <= DivAssign))
                ast_Tk(n) += 5;
            typecheck(ast_Tk(n), t, ty);
            ast_Assign((int)b, (ty << 16) | t);
            ty = t;
            break;
        case Cond: // `x?a:b` is similar to if except that it relies on else
            next();
            expr(Assign);
            tc = ty;
            if (tk != ':')
                fatal("conditional missing colon");
            next();
            c = n;
            expr(Cond);
            if (tc != ty)
                fatal("both results need same type");
            ast_Cond((int)n, (int)c, (int)b);
            break;
        case Lor: // short circuit, the logical or
            next();
            expr(Lan);
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                ast_NumVal(b) = ast_NumVal(b) || ast_NumVal(n);
                n = b;
            } else {
                ast_Oper((int)b, Lor);
            }
            ty = INT;
            break;
        case Lan: // short circuit, logic and
            next();
            expr(Or);
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                ast_NumVal(b) = ast_NumVal(b) && ast_NumVal(n);
                n = b;
            } else {
                ast_Oper((int)b, Lan);
            }
            ty = INT;
            break;
        case Or: // push the current value, calculate the right value
            next();
            expr(Xor);
            bitopcheck(t, ty);
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                ast_NumVal(b) = ast_NumVal(b) | ast_NumVal(n);
                n = b;
            } else {
                ast_Oper((int)b, Or);
            }
            ty = INT;
            break;
        case Xor:
            next();
            expr(And);
            bitopcheck(t, ty);
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                ast_NumVal(b) = ast_NumVal(b) ^ ast_NumVal(n);
                n = b;
            } else {
                ast_Oper((int)b, Xor);
            }
            ty = INT;
            break;
        case And:
            next();
            expr(Eq);
            bitopcheck(t, ty);
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                ast_NumVal(b) = ast_NumVal(b) & ast_NumVal(n);
                n = b;
            } else
                ast_Oper((int)b, And);
            ty = INT;
            break;
        case Eq:
            next();
            expr(Ge);
            typecheck(Eq, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    ast_NumVal(b) = ast_NumVal(n) == ast_NumVal(b);
                    ast_Tk(b) = Num;
                    n = b;
                } else
                    ast_Oper((int)b, EqF);
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    ast_NumVal(b) = ast_NumVal(b) == ast_NumVal(n);
                    n = b;
                } else
                    ast_Oper((int)b, Eq);
            }
            ty = INT;
            break;
        case Ne:
            next();
            expr(Ge);
            typecheck(Ne, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    ast_NumVal(b) = ast_NumVal(n) != ast_NumVal(b);
                    ast_Tk(b) = Num;
                    n = b;
                } else
                    ast_Oper((int)b, NeF);
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    ast_NumVal(b) = ast_NumVal(b) != ast_NumVal(n);
                    n = b;
                } else {
                    ast_Oper((int)b, Ne);
                }
            }
            ty = INT;
            break;
        case Ge:
            next();
            expr(Shl);
            typecheck(Ge, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    ast_NumVal(b) = (*((float*)&ast_NumVal(b)) >= *((float*)&ast_NumVal(n)));
                    ast_Tk(b) = Num;
                    n = b;
                } else {
                    ast_Oper((int)b, GeF);
                }
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    ast_NumVal(b) = ast_NumVal(b) >= ast_NumVal(n);
                    n = b;
                } else
                    ast_Oper((int)b, Ge);
            }
            ty = INT;
            break;
        case Lt:
            next();
            expr(Shl);
            typecheck(Lt, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    ast_NumVal(b) = (*((float*)&ast_NumVal(b)) < *((float*)&ast_NumVal(n)));
                    ast_Tk(b) = Num;
                    n = b;
                } else
                    ast_Oper((int)b, LtF);
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    ast_NumVal(b) = ast_NumVal(b) < ast_NumVal(n);
                    n = b;
                } else
                    ast_Oper((int)b, Lt);
            }
            ty = INT;
            break;
        case Gt:
            next();
            expr(Shl);
            typecheck(Gt, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    ast_NumVal(b) = (*((float*)&ast_NumVal(b)) > *((float*)&ast_NumVal(n)));
                    ast_Tk(b) = Num;
                    n = b;
                } else
                    ast_Oper((int)b, GtF);
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    ast_NumVal(b) = ast_NumVal(b) > ast_NumVal(n);
                    n = b;
                } else
                    ast_Oper((int)b, Gt);
            }
            ty = INT;
            break;
        case Le:
            next();
            expr(Shl);
            typecheck(Le, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    ast_NumVal(b) = (*((float*)&ast_NumVal(b)) <= *((float*)&ast_NumVal(n)));
                    ast_Tk(b) = Num;
                    n = b;
                } else
                    ast_Oper((int)b, LeF);
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    ast_NumVal(b) = ast_NumVal(b) <= ast_NumVal(n);
                    n = b;
                } else
                    ast_Oper((int)b, Le);
            }
            ty = INT;
            break;
        case Shl:
            next();
            expr(Add);
            bitopcheck(t, ty);
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                ast_NumVal(b) = (ast_NumVal(n) < 0) ? ast_NumVal(b) >> -ast_NumVal(n)
                                                    : ast_NumVal(b) << ast_NumVal(n);
                n = b;
            } else
                ast_Oper((int)b, Shl);
            ty = INT;
            break;
        case Shr:
            next();
            expr(Add);
            bitopcheck(t, ty);
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                ast_NumVal(b) = (ast_NumVal(n) < 0) ? ast_NumVal(b) << -ast_NumVal(n)
                                                    : ast_NumVal(b) >> ast_NumVal(n);
                n = b;
            } else
                ast_Oper((int)b, Shr);
            ty = INT;
            break;
        case Add:
            next();
            expr(Mul);
            typecheck(Add, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    *((float*)&ast_NumVal(b)) =
                        (*((float*)&ast_NumVal(b)) + *((float*)&ast_NumVal(n)));
                    n = b;
                } else
                    ast_Oper((int)b, AddF);
            } else { // both terms are either int or "int *"
                tc = ((t | ty) & (PTR | PTR2)) ? (t >= PTR) : (t >= ty);
                c = n;
                if (tc)
                    ty = t;
                sz = (ty >= PTR2) ? sizeof(int) : ((ty >= PTR) ? tsize[(ty - PTR) >> 2] : 1);
                if (ast_Tk(n) == Num && tc) {
                    ast_NumVal(n) *= sz;
                    sz = 1;
                } else if (ast_Tk(b) == Num && !tc) {
                    ast_NumVal(b) *= sz;
                    sz = 1;
                }
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    ast_NumVal(b) += ast_NumVal(n);
                    n = b;
                } else if (sz != 1) {
                    ast_Num(sz);
                    ast_Oper((int)(tc ? c : b), Mul);
                    ast_Oper((int)(tc ? b : c), Add);
                } else
                    ast_Oper((int)b, Add);
            }
            break;
        case Sub:
            next();
            expr(Mul);
            typecheck(Sub, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    *((float*)&ast_NumVal(b)) =
                        (*((float*)&ast_NumVal(b)) - *((float*)&ast_NumVal(n)));
                    n = b;
                } else
                    ast_Oper((int)b, SubF);
            } else {            // 4 cases: ptr-ptr, ptr-int, int-ptr (err), int-int
                if (t >= PTR) { // left arg is ptr
                    sz = (t >= PTR2) ? sizeof(int) : tsize[(t - PTR) >> 2];
                    if (ty >= PTR) { // ptr - ptr
                        if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                            ast_NumVal(b) = (ast_NumVal(b) - ast_NumVal(n)) / sz;
                            n = b;
                        } else {
                            ast_Oper((int)b, Sub);
                            if (sz > 1) {
                                if (is_power_of_2(sz)) { // 2^n
                                    ast_Num(__builtin_popcount(sz - 1));
                                    ast_Oper((int)(n + 2), Shr);
                                } else {
                                    ast_Num(sz);
                                    ast_Oper((int)(n + 2), Div);
                                }
                            }
                        }
                        ty = INT;
                    } else { // ptr - int
                        if (ast_Tk(n) == Num) {
                            ast_NumVal(n) *= sz;
                            if (ast_Tk(b) == Num) {
                                ast_NumVal(b) = ast_NumVal(b) - ast_NumVal(n);
                                n = b;
                            } else {
                                ast_Oper((int)b, Sub);
                            }
                        } else {
                            if (sz > 1) {
                                if (is_power_of_2(sz)) { // 2^n
                                    ast_Num(__builtin_popcount(sz - 1));
                                    ast_Oper((int)(n + 2), Shl);
                                } else {
                                    ast_Num(sz);
                                    ast_Oper((int)(n + 2), Mul);
                                }
                            }
                            ast_Oper((int)b, Sub);
                        }
                        ty = t;
                    }
                } else { // int - int
                    if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                        ast_NumVal(b) = ast_NumVal(b) - ast_NumVal(n);
                        n = b;
                    } else
                        ast_Oper((int)b, Sub);
                    ty = INT;
                }
            }
            break;
        case Mul:
            next();
            expr(Inc);
            typecheck(Mul, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    *((float*)&ast_NumVal(b)) *= *((float*)&ast_NumVal(n));
                    n = b;
                } else
                    ast_Oper((int)b, MulF);
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    ast_NumVal(b) *= ast_NumVal(n);
                    n = b;
                } else {
                    if (ast_Tk(n) == Num && ast_NumVal(n) > 0 && is_power_of_2(ast_NumVal(n))) {
                        ast_NumVal(n) = __builtin_popcount(ast_NumVal(n) - 1);
                        ast_Oper((int)b, Shl); // 2^n
                    } else
                        ast_Oper((int)b, Mul);
                }
                ty = INT;
            }
            break;
        case Inc:
        case Dec:
            if (ty & 3)
                fatal("can't inc/dec an array variable");
            if (ty == FLOAT)
                fatal("no ++/-- on float");
            sz = (ty >= PTR2) ? sizeof(int) : ((ty >= PTR) ? tsize[(ty - PTR) >> 2] : 1);
            if (ast_Tk(n) != Load)
                fatal("bad lvalue in post-increment");
            ast_Tk(n) = tk;
            ast_Num(sz);
            ast_Oper((int)b, (tk == Inc) ? Sub : Add);
            next();
            break;
        case Div:
            next();
            expr(Inc);
            typecheck(Div, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    *((float*)&ast_NumVal(b)) =
                        (*((float*)&ast_NumVal(b)) / *((float*)&ast_NumVal(n)));
                    n = b;
                } else
                    ast_Oper((int)b, DivF);
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    ast_NumVal(b) /= ast_NumVal(n);
                    n = b;
                } else {
                    if (ast_Tk(n) == Num && ast_NumVal(n) > 0 && is_power_of_2(ast_NumVal(n))) {
                        ast_NumVal(n) = __builtin_popcount(ast_NumVal(n) - 1);
                        ast_Oper((int)b, Shr); // 2^n
                    } else
                        ast_Oper((int)b, Div);
                }
                ty = INT;
            }
            break;
        case Mod:
            next();
            expr(Inc);
            typecheck(Mod, t, ty);
            if (ty == FLOAT)
                fatal("use fmodf() for float modulo");
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                ast_NumVal(b) %= ast_NumVal(n);
                n = b;
            } else {
                if (ast_Tk(n) == Num && ast_NumVal(n) > 0 && is_power_of_2(ast_NumVal(n))) {
                    --ast_NumVal(n);
                    ast_Oper((int)b, And); // 2^n
                } else
                    ast_Oper((int)b, Mod);
            }
            ty = INT;
            break;
        case Dot:
            t += PTR;
            if (ast_Tk(n) == Load && ast_NumVal(n) > ATOM_TYPE && ast_NumVal(n) < PTR)
                n += 2; // struct
        case Arrow:
            if (t <= PTR + ATOM_TYPE || t >= PTR2)
                fatal("structure expected");
            next();
            if (tk != Id)
                fatal("structure member expected");
            m = members[(t - PTR) >> 2];
            while (m && m->id != id)
                m = m->next;
            if (!m)
                fatal("structure member not found");
            if (m->offset) {
                ast_Num(m->offset);
                ast_Oper((int)(n + 2), Add);
            }
            ty = m->type;
            next();
            if (!(ty & 3)) {
                ast_Oper((ty >= PTR) ? INT : ty, Load);
                break;
            }
            memsub = 1;
            int dim = ty & 3, ee = m->etype;
            b = n;
            t = ty & ~3;
        case Bracket:
            if (t < PTR)
                fatal("pointer type expected");
            if (memsub == 0) {
                dim = id->type & 3, ee = id->etype;
            }
            int sum = 0, ii = dim - 1, *f = 0;
            int doload = 1;
            memsub = 0;
            sz = ((t = t - PTR) >= PTR) ? sizeof(int) : tsize[t >> 2];
            do {
                if (dim && tk != Bracket) { // ptr midway for partial subscripting
                    t += PTR * (ii + 1);
                    doload = 0;
                    break;
                }
                next();
                expr(Assign);
                if (ty >= FLOAT)
                    fatal("non-int array index");
                if (tk != ']')
                    fatal("close bracket expected");
                c = n;
                next();
                if (dim) {
                    int factor = ((ii == 2) ? (((ee >> 11) & 0x3ff) + 1) : 1);
                    factor *=
                        ((dim == 3 && ii >= 1) ? ((ee & 0x7ff) + 1)
                                               : ((dim == 2 && ii == 1) ? ((ee & 0xffff) + 1) : 1));
                    if (ast_Tk(n) == Num) {
                        // elision with struct offset for efficiency
                        if (ast_Tk(b) == Add && ast_Tk(b + 1) == Num)
                            ast_NumVal(b + 1) += factor * ast_NumVal(n) * sz;
                        else
                            sum += factor * ast_NumVal(n);
                        n += sizeof(Double_entry_t) / sizeof(int); // delete the subscript constant
                    } else {
                        // generate code to add a term
                        if (factor > 1) {
                            ast_Num(factor);
                            ast_Oper((int)c, Mul);
                        }
                        if (f)
                            ast_Oper((int)f, Add);
                        f = n;
                    }
                }
            } while (--ii >= 0);
            if (dim) {
                if (sum != 0) {
                    if (f) {
                        ast_Num(sum);
                        ast_Oper((int)f, Add);
                    } else {
                        sum *= sz;
                        sz = 1;
                        ast_Num(sum);
                    }
                } else if (!f)
                    goto add_simple;
            }
            if (sz > 1) {
                if (ast_Tk(n) == Num)
                    ast_NumVal(n) *= sz;
                else {
                    ast_Num(sz);
                    ast_Oper((int)(n + 2), Mul);
                }
            }
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                ast_NumVal(b) += ast_NumVal(n);
                n = b;
            } else
                ast_Oper((int)b, Add);
        add_simple:
            if (doload)
                ast_Load(((ty = t) >= PTR) ? INT : ty);
            break;
        default:
            fatal("%d: compiler error tk=%d\n", line, tk);
        }
    }
}
static void init_array(struct ident_s* tn, int extent[], int dim) {
    int i, cursor, match, coff = 0, off, empty, *vi;
    int inc[3];

    inc[0] = extent[dim - 1];
    for (i = 1; i < dim; ++i)
        inc[i] = inc[i - 1] * extent[dim - (i + 1)];

    // Global is preferred to local.
    // Either suggest global or automatically move to global scope.
    if (tn->class != Glo)
        fatal("only global array initialization supported");

    switch (tn->type & ~3) {
    case (CHAR | PTR2):
        match = CHAR + PTR2;
        break;
    case (CHAR | PTR):
        match = CHAR + PTR;
        coff = 1;
        break; // strings
    case (INT | PTR):
        match = INT;
        break;
    case (FLOAT | PTR):
        match = FLOAT;
        break;
    default:
        fatal("array-init must be literal ints, floats, or strings");
    }

    vi = (int*)tn->val;
    i = 0;
    cursor = (dim - coff);
    do {
        if (tk == '{') {
            next();
            if (cursor)
                --cursor;
            else
                fatal("overly nested initializer");
            empty = 1;
            continue;
        } else if (tk == '}') {
            next();
            // skip remainder elements on this level (or set 0 if cmdline opt)
            if ((off = i % inc[cursor + coff]) || empty)
                i += (inc[cursor + coff] - off);
            if (++cursor == dim - coff)
                break;
        } else {
            expr(Cond);
            if (ast_Tk(n) != Num && ast_Tk(n) != NumF)
                fatal("non-literal initializer");

            if (ty == CHAR + PTR) {
                if (match == CHAR + PTR2) {
                    vi[i++] = ast_NumVal(n);
                } else if (match == CHAR + PTR) {
                    off = strlen((char*)ast_NumVal(n)) + 1;
                    if (off > inc[0]) {
                        off = inc[0];
                        printf("%d: string '%s' truncated to %d chars\n", line,
                               (char*)ast_NumVal(n), off);
                    }
                    memcpy((char*)vi + i, (char*)ast_NumVal(n), off);
                    i += inc[0];
                } else
                    fatal("can't assign string to scalar");
            } else if (ty == match)
                vi[i++] = ast_NumVal(n);
            else if (ty == INT) {
                if (match == CHAR + PTR) {
                    *((char*)vi + i) = ast_NumVal(n);
                    i += inc[0];
                } else {
                    *((float*)(n + 1)) = (float)ast_NumVal(n);
                    vi[i++] = ast_NumVal(n);
                }
            } else if (ty == FLOAT) {
                if (match == INT) {
                    vi[i++] = (int)*((float*)(n + 1));
                } else
                    fatal("illegal char/string initializer");
            }
            n += 2; // clean up AST
            empty = 0;
        }
        if (tk == ',')
            next();
    } while (1);
}

static void old_emit(int n) {
    // e = 0;
}

static void emit(uint16_t n) {
    if (e >= text_base + (TEXT_BYTES / sizeof(*e)))
        fatal("code segment exceeded, program is too big");
    *++e = n;
}

static void emit_Word(uint32_t n) {
    if (((int)e & 2) == 0)
        fatal("mis-aligned word");
    ++e;
    if (e >= text_base + (TEXT_BYTES / sizeof(*e)) - 1)
        fatal("code segment exceeded, program is too big");
    *((uint32_t*)e) = n;
    ++e;
}

static void new_lit(uint16_t* addr, int val, int type) {
    struct patch_s* l = sys_malloc(sizeof(struct patch_s), 1);
    l->addr = addr;
    l->val = val;
    l->patch_type = type;
    l->next = lit;
    lit = l;
}

static void emit_ENT(int n) {
    emit(0xb5f0);             // push    {r4, r5, r6, r7, lr}
    emit(0x466f);             // mov  r7, sp
    if (n) {                  //
        if (n < 128)          //
            emit(0xb080 | n); // sub  sp, #n
        else {                //
            emit(0x4900);     // ldr r1, [pc, n]
            emit(0x448d);     // add sp, r1
            new_lit(e - 1, loc - ld, pc_relative);
        }
    }
}

static void emit_LEV(void) {
    emit(0x46bd);          // mov sp, r7
    emit(0xbdf0);          // pop {r4, r5, r6, r7, pc}
    if (((int)e & 2) == 0) //
        emit(0x46c0);      // nop ; (mov r8, r8)
}

static void patch_literals(void) {
    while (lit) {
        emit_Word(lit->val);
        switch (lit->patch_type) {
        case pc_relative:
            int v = ((int)e - ((int)lit->addr & ~3)) / 4 - 1;
            if (v >= 256)
                fatal("pc relative too far");
            *lit->addr |= v;
            break;
        default:
            fatal("unexpected compiler error");
        }
        struct patch_s* tlit = lit->next;
        sys_free(lit);
        lit = tlit;
    }
}

static void emit_IMM(int n) {
    if (n < 256)          //
        emit(0x2000 | n); // movs r0, #n
    else {                //
        emit(0x4800);     // ldr r0, [pc, n]
        new_lit(e, n, pc_relative);
    }
}

static void emit_IMMF(int n) { emit_IMM(n); }

static void emit_LEA(int n) {
    emit_IMM((-n + 5) * 4);
    emit(0x4438); // add r0, r7
}

static void emit_PSH(void) {
    emit(0xb401); // push {r0}
}

static void emit_S(int n) {
    emit(0xbc02); // pop {r1}
    switch (n) {
    case SC:
        emit(0x7008); // strb    r0, [r1, #0]
        break;
    case SI:
    case SF:
        emit(0x6008); // str r0, [r1, #0]
        break;
    default:
        fatal("unexpected compiler error");
    }
}

static void emit_JMP(int n) {
    int ofs = 0;
    if (n) {
        ofs = (n - (int)e) / 2 - 1;
        if (ofs < -1024 || ofs > 1023)
            fatal("jmp too far");
        ofs &= 0x7ff;
    }
    emit(0xe000 | ofs); // JMP n
}

static void emit_B(int n, int cond) {
    uint16_t i;
    switch (cond) {
    case BZ:
        i = 0xd000; // beq n
        break;
    case BNZ:
        i = 0xd100; // bne n
        break;
    default:
        fatal("unexpected compiler error");
    }
    int ofs = 0;
    if (n) {
        ofs = (n - (int)e) / 2 - 1;
        if (ofs < -128 || ofs > 127)
            fatal("branch too far");
        ofs &= 0xff;
    }
    emit(i | ofs);
}

static void emit_OP(int op) {
    switch (op) {
    case OR:
        emit(0xbc02); // pop {r1}
        emit(0x4308);
        break;
    case XOR:
        emit(0xbc02); // pop {r1}
        emit(0x4048);
        break;
    case AND:
        emit(0xbc02); // pop {r1}
        emit(0x4008);
        break;
    case SHL:
        emit(0xbc02); // pop {r1}
        emit(0x4088);
        break;
    case SHR:
        emit(0xbc02); // pop {r1}
        emit(0x4108);
        break;
    case SUB:
        emit(0xbc02); // pop {r1}
        emit(0x1a40); // subs    r0, r0, r1;
        break;
    case ADD:
        emit(0xbc02); // pop {r1}
        emit(0x1840); // adds    r0, r0, r1;
        break;
    case MUL:
        emit(0xbc02); // pop {r1}
        emit(0x4348); // muls    r0, r1;
        break;

    case EQ:
    case NE:
    case GE:
    case LT:
    case GT:
    case LE:
        emit(0xbc02); // pop {r1}
        emit(0x4288); // cmp r0, r1
        emit_IMM(1);
        uint16_t i;
        switch (op) {
        case EQ:
            emit(0xd001); // beq * + 2
            break;
        case NE:
            emit(0xd101); // bne * + 2
            break;
        case GE:
            emit(0xda01); // bge * + 2
            break;
        case LT:
            emit(0xdb01); // blt * + 2
            break;
        case GT:
            emit(0xdc01); // bgt * + 2
            break;
        case LE:
            emit(0xdd01); // ble * + 2
            break;
        default:
            fatal("unexpected compiler error");
        }
        emit_IMM(0);
        break;

    case DIV:
    case MOD:
        emit(0xbc02); // pop {r1}
        emit(0x4a00); // ldr r2, [pc, n]
        new_lit(e, (int)__wrap___aeabi_idiv, pc_relative);
        emit(0x4790); // blx r2
        if (op == MOD)
            emit(0x4608); // mov r0,r1
        break;
    default:
        fatal("unexpected compiler error");
    }
}

static void emit_OP_F(int op) {
    switch (op) {
    case ADDF:
    case SUBF:
    case MULF:
    case DIVF:
        emit(0xbc02); // pop {r1}
        emit(0x4a00); // ldr r2, [pc, n]
        switch (op) {
        case ADDF:
            new_lit(e, (int)__wrap___aeabi_fadd, pc_relative);
            break;
        case SUBF:
            new_lit(e, (int)__wrap___aeabi_fsub, pc_relative);
            break;
        case MULF:
            new_lit(e, (int)__wrap___aeabi_fmul, pc_relative);
            break;
        case DIVF:
            new_lit(e, (int)__wrap___aeabi_fdiv, pc_relative);
            break;
        }
        emit(0x4790); // blx r2

    case EQF:
        emit_OP(EQ);
        break;
    case NEF:
        emit_OP(NE);
        break;

    case GEF:
    case LTF:
    case GTF:
    case LEF:
        emit(0xbc02); // pop {r1}
        emit(0x4a00); // ldr r2, [pc, n]
        switch (op) {
        case GEF:
            new_lit(e, (int)__wrap___aeabi_fcmpge, pc_relative);
            break;
        case LTF:
            new_lit(e, (int)__wrap___aeabi_fcmplt, pc_relative);
            break;
        case GTF:
            new_lit(e, (int)__wrap___aeabi_fcmpgt, pc_relative);
            break;
        case LEF:
            new_lit(e, (int)__wrap___aeabi_fcmple, pc_relative);
            break;
        default:
            fatal("unexpected compiler error");
        }
        emit(0x4790); // blx r2
        break;

    default:
        fatal("unexpected compiler error");
    }
}

// AST parsing for IR generatiion
// With a modular code generator, new targets can be easily supported such as
// native Arm machine code.
static void gen(int* n) {
    int i = ast_Tk(n), j, k, l;
    uint16_t *a, *b, *c, *d, *t;
    struct ident_s* label;
    struct patch_s* patch;

    switch (i) {
    case Num:
        emit_IMM(ast_NumVal(n));
        break; // int value
    case NumF:
        emit_IMMF(ast_NumVal(n));
        break; // float value
    case Load:
        gen(n + 2);                                           // load the value
        if (ast_NumVal(n) > ATOM_TYPE && ast_NumVal(n) < PTR) // unreachable?
            fatal("struct copies not yet supported");
        old_emit((ast_NumVal(n) >= PTR) ? LI : LC + (ast_NumVal(n) >> 2));
        break;
    case Loc:
        emit_LEA(ast_NumVal(n));
        break; // get address of variable
    case '{':
        gen((int*)ast_NumVal(n));
        gen(n + 2);
        break;   // parse AST expr or stmt
    case Assign: // assign the value to variables
        gen((int*)Assign_entry(n).right_part);
        emit_PSH();
        gen(n + 3);
        l = ast_NumVal(n) & 0xffff;
        // Add SC/SI instruction to save value in register to variable address
        // held on stack.
        if (l > ATOM_TYPE && l < PTR)
            fatal("struct assign not yet supported");
        if ((ast_NumVal(n) >> 16) == FLOAT && l == INT)
            old_emit(FTOI);
        else if ((ast_NumVal(n) >> 16) == INT && l == FLOAT)
            old_emit(ITOF);
        emit_S((l >= PTR) ? SI : SC + (l >> 2));
        break;
    case Inc: // increment or decrement variables
    case Dec:
        gen(n + 2);
        emit_PSH();
        old_emit((ast_NumVal(n) == CHAR) ? LC : LI);
        emit_PSH();
        emit_IMM((ast_NumVal(n) >= PTR2)
                     ? sizeof(int)
                     : ((ast_NumVal(n) >= PTR) ? tsize[(ast_NumVal(n) - PTR) >> 2] : 1));
        emit_OP((i == Inc) ? ADD : SUB);
        emit_S((ast_NumVal(n) == CHAR) ? SC : SI);
        break;
    case Cond:                              // if else condition case
        gen((int*)Cond_entry(n).cond_part); // condition
        // Add jump-if-zero instruction "BZ" to jump to false branch.
        // Point "b" to the jump address field to be patched later.
        emit_B(0, BZ);
        b = e;
        gen((int*)Cond_entry(n).if_part); // expression
        // Patch the jump address field pointed to by "b" to hold the address
        // of false branch. "+ 3" counts the "JMP" instruction added below.
        //
        // Add "JMP" instruction after true branch to jump over false branch.
        // Point "b" to the jump address field to be patched later.
        if (Cond_entry(n).else_part) {
            //*b = (int)(e + 3); // *** FIX ***
            emit_JMP(0);
            b = e;
            gen((int*)Cond_entry(n).else_part);
        } // else statment
        // Patch the jump address field pointed to by "d" to hold the address
        // past the false branch.
        //*b = (int)(e + 1); // ***FIX***
        break;
    // operators
    /* If current token is logical OR operator:
     * Add jump-if-nonzero instruction "BNZ" to implement short circuit.
     * Point "b" to the jump address field to be patched later.
     * Parse RHS expression.
     * Patch the jump address field pointed to by "b" to hold the address past
     * the RHS expression.
     */
    case Lor:
        gen((int*)ast_NumVal(n));
        emit_B(0, BNZ);
        b = e;
        gen(n + 2);
        ast_Tk(b) = (int)(e + 1);
        break;
    case Lan:
        gen((int*)ast_NumVal(n));
        emit_B(0, BZ);
        b = e;
        gen(n + 2);
        ast_Tk(b) = (int)(e + 1);
        break;
    /* If current token is bitwise OR operator:
     * Add "PSH" instruction to push LHS value in register to stack.
     * Parse RHS expression.
     * Add "OR" instruction to compute the result.
     */
    case Or:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(OR);
        break;
    case Xor:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(XOR);
        break;
    case And:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(AND);
        break;
    case Eq:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(EQ);
        break;
    case Ne:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(NE);
        break;
    case Ge:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(GE);
        break;
    case Lt:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(LT);
        break;
    case Gt:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(GT);
        break;
    case Le:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(LE);
        break;
    case Shl:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(SHL);
        break;
    case Shr:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(SHR);
        break;
    case Add:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(ADD);
        break;
    case Sub:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(SUB);
        break;
    case Mul:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(MUL);
        break;
    case Div:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(DIV);
        break;
    case Mod:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP(MOD);
        break;
    case AddF:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP_F(ADDF);
        break;
    case SubF:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP_F(SUBF);
        break;
    case MulF:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP_F(MULF);
        break;
    case DivF:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP_F(DIVF);
        break;
    case EqF:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP_F(EQF);
        break;
    case NeF:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP_F(NEF);
        break;
    case GeF:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP_F(GEF);
        break;
    case LtF:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP_F(LTF);
        break;
    case GtF:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP_F(GTF);
        break;
    case LeF:
        gen((int*)ast_NumVal(n));
        emit_PSH();
        gen(n + 2);
        emit_OP_F(LEF);
        break;
    case CastF:
        gen((int*)CastF_entry(n).val);
        old_emit(CastF_entry(n).way);
        break;
    case Func:
    case Syscall:
        b = (uint16_t*)Func_entry(n).next;
        k = b ? Func_entry(n).n_parms : 0;
        if (k) {
            l = Func_entry(n).parm_types >> 10;
            int* t;
            t = sys_malloc(sizeof(int) * (k + 1), 1);
            j = 0;
            while (ast_Tk(b)) {
                t[j++] = (int)b;
                b = (uint16_t*)ast_Tk(b);
            }
            int sj = j;
            while (j >= 0) { // push arguments
                gen((int*)b + 1);
                emit_PSH();
                --j;
                b = (uint16_t*)t[j];
            }
            sys_free(t);
            if (i == Syscall)
                emit_IMM((sj + 1) | ((Func_entry(n).parm_types >> 10) << 10));
        }
        if (i == Syscall)
            old_emit(SYSC);
        if (i == Func)
            old_emit(JSR);
        old_emit(Func_entry(n).addr);
        if (Func_entry(n).n_parms) {
            old_emit(ADJ);
            old_emit((i == Syscall) ? Func_entry(n).parm_types : Func_entry(n).n_parms);
        }
        break;
    case While:
    case DoWhile:
        if (i == While) {
            emit_JMP(0);
            a = e;
        }
        d = (e + 1);
        b = (uint16_t*)brks;
        brks = 0;
        c = (uint16_t*)cnts;
        cnts = 0;
        gen((int*)While_entry(n).body); // loop body
        // if (i == While) // *** FIX ***
        //    *a = (int)(e + 1);
        while (cnts) {
            t = (uint16_t*)cnts->next;
            //*cnts->addr = (int)(e + 1); // *** FIX ***
            sys_free(cnts);
            cnts = (struct patch_s*)t;
        }
        cnts = (struct patch_s*)c;
        gen((int*)While_entry(n).cond); // condition
        emit_B((int)d, BNZ);
        while (brks) {
            t = (uint16_t*)brks->next;
            // *brks->addr = (int)(e + 1); // ***FIX***
            sys_free(brks);
            brks = (struct patch_s*)t;
        }
        brks = (struct patch_s*)b;
        break;
    case For:
        gen((int*)For_entry(n).init); // init
        emit_JMP(0);
        a = e;
        d = (e + 1);
        b = (uint16_t*)brks;
        brks = 0;
        c = (uint16_t*)cnts;
        cnts = 0;
        gen((int*)For_entry(n).body); // loop body
        while (cnts) {
            t = (uint16_t*)cnts->next;
            //*cnts->addr = (int)(e + 1); // ***FIX ***
            sys_free(cnts);
            cnts = (struct patch_s*)t;
        }
        cnts = (struct patch_s*)c;
        gen((int*)For_entry(n).incr); // increment
        //*a = (int)(e + 1); // *** FIX ***
        if (For_entry(n).cond) {
            gen((int*)For_entry(n).cond); // condition
            emit_B((int)d, BNZ);
        } else {
            emit_JMP((int)d);
        }
        while (brks) {
            t = (uint16_t*)brks->next;
            //*brks->addr = (int)(e + 1); // *** FIX ***
            sys_free(brks);
            brks = (struct patch_s*)t;
        }
        brks = (struct patch_s*)b;
        break;
    case Switch:
        gen((int*)Switch_entry(n).cond); // condition
        a = (uint16_t*)cas;
        emit_JMP(0);
        cas = (int*)e;
        b = (uint16_t*)brks;
        d = (uint16_t*)def;
        def = 0;
        brks = 0;
        gen((int*)Switch_entry(n).cas); // case statment
        // deal with no default inside switch case
        if (def) {
            t = (uint16_t*)def->next;
            //*cas = (int)def->addr; // ***FIX***
            sys_free(def);
            def = (struct patch_s*)t;
        } else
            //*cas = (int)(e + 1); // ***FIX***
            cas = (int*)a;
        while (brks) {
            t = (uint16_t*)brks->next;
            //*brks->addr = (int)(e + 1); // ***FIX***
            sys_free(brks);
            brks = (struct patch_s*)t;
        }
        brks = (struct patch_s*)b;
        def = (struct patch_s*)d;
        break;
    case Case:
        emit_JMP(0);
        a = 0;
        //*e = (int)(e + 7); // ***FIX***
        emit_PSH();
        i = *cas;
        //*cas = (int)e; // ***FIX***
        gen((int*)ast_NumVal(n)); // condition
        // if (*(e - 1) != IMM) // ***FIX***
        //    fatal("case label not a numeric literal");
        emit_OP(SUB);
        emit_B(0, BNZ);
        cas = (int*)e;
        //*e = i + e[-3]; // ***FIX***
        // if (*((int*)Case_entry(n).expr) == Switch) // ***FIX***
        //    a = (uint16_t*)cas;
        gen((int*)Case_entry(n).expr); // expression
        // if (a != 0) // ***FIX***
        //    cas = (int*)a;
        break;
    case Break:
        emit_JMP(0);
        patch = sys_malloc(sizeof(struct patch_s), 1);
        patch->addr = e;
        patch->next = brks;
        brks = patch;
        break;
    case Continue:
        emit_JMP(0);
        patch = sys_malloc(sizeof(struct patch_s), 1);
        patch->next = cnts;
        patch->addr = e;
        cnts = patch;
        break;
    case Goto:
        label = (struct ident_s*)ast_NumVal(n);
        emit_JMP(label->val);
        if (label->class == 0)
            label->val = (int)e; // Define label address later
        break;
    case Default:
        patch = sys_malloc(sizeof(struct patch_s), 1);
        patch->next = def;
        patch->addr = e + 1;
        def = patch;
        gen((int*)ast_NumVal(n));
        break;
    case Return:
        if (ast_NumVal(n))
            gen((int*)ast_NumVal(n));
        emit_LEV();
        break;
    case Enter:
        emit_ENT(ast_NumVal(n));
        gen(n + 2);
        if (*(e - 2) != 0x46bd && *(e - 1) != 0xbdf0)
            emit_LEV(); // don't issue it again if already emitted by return stmt
        break;
    case Label: // target of goto
        label = (struct ident_s*)ast_NumVal(n);
        if (label->class != 0)
            fatal("duplicate label definition");
        d = e + 1;
        b = (uint16_t*)label->val;
        while (b != 0) {
            t = (uint16_t*)ast_Tk(b);
            ast_Tk(b) = (int)d;
            b = t;
        }
        label->val = (int)d;
        label->class = Label;
        break;
    default:
        if (i != ';')
            fatal("%d: compiler error gen=%08x\n", line, i);
    }
}

static void check_label(int** tt) {
    if (tk != Id)
        return;
    char* ss = p;
    while (*ss == ' ' || *ss == '\t')
        ++ss;
    if (*ss == ':') {
        if (id->class != 0 || !(id->type == 0 || id->type == -1))
            fatal("invalid label");
        id->type = -1; // hack for id->class deficiency
        ast_Label((int)id);
        ast_Begin((int)*tt);
        *tt = n;
        next();
        next();
    }
}

static void loc_array_decl(int ct, int extent[3], int* dims, int* et, int* size) {
    *dims = 0;
    do {
        next();
        if (*dims == 0 && ct == Par && tk == ']') {
            extent[*dims] = 1;
            next();
        } else {
            expr(Cond);
            if (ast_Tk(n) != Num)
                fatal("non-const array size");
            if (ast_NumVal(n) <= 0)
                fatal("non-positive array dimension");
            if (tk != ']')
                fatal("missing ]");
            next();
            extent[*dims] = ast_NumVal(n);
            *size *= ast_NumVal(n);
            n += 2;
        }
        ++*dims;
    } while (tk == Bracket && *dims < 3);
    if (tk == Bracket)
        fatal("three subscript max on decl");
    switch (*dims) {
    case 1:
        *et = (extent[0] - 1);
        break;
    case 2:
        *et = ((extent[0] - 1) << 16) + (extent[1] - 1);
        if (extent[0] > 32768 || extent[1] > 65536)
            fatal("max bounds [32768][65536]");
        break;
    case 3:
        *et = ((extent[0] - 1) << 21) + ((extent[1] - 1) << 11) + (extent[2] - 1);
        if (extent[0] > 1024 || extent[1] > 1024 || extent[2] > 2048)
            fatal("max bounds [1024][1024][2048]");
        break;
    }
}

// statement parsing (syntax analysis, except for declarations)
static void stmt(int ctx) {
    struct ident_s* dd;
    int *a, *b, *c, *d;
    int i, j, nf, atk, sz;
    int nd[3];
    int bt;

    if (ctx == Glo && (tk < Enum || tk > Union))
        fatal("syntax: statement used outside function");

    switch (tk) {
    case Enum:
        next();
        // If current token is not "{", it means having enum type name.
        // Skip the enum type name.
        if (tk == Id)
            next();
        if (tk == '{') {
            next();
            i = 0; // Enum value starts from 0
            while (tk != '}') {
                // Current token should be enum name.
                // If current token is not identifier, stop parsing.
                if (tk != Id)
                    fatal("bad enum identifier");
                dd = id;
                next();
                if (tk == Assign) {
                    next();
                    expr(Cond);
                    if (ast_Tk(n) != Num)
                        fatal("bad enum initializer");
                    i = ast_NumVal(n);
                    n += 2; // Set enum value
                }
                dd->class = Num;
                dd->type = INT;
                dd->val = i++;
                if (tk == ',')
                    next(); // If current token is ",", skip.
            }
            next(); // Skip "}"
        } else if (tk == Id) {
            if (ctx != Par)
                fatal("enum can only be declared as parameter");
            id->type = INT;
            id->class = ctx;
            id->val = ld++;
            ir_var[ir_count++] = id;
            next();
        }
        return;
    case Char:
    case Int:
    case Float:
    case Struct:
    case Union:
        dd = id;
        switch (tk) {
        case Char:
        case Int:
        case Float:
            bt = (tk - Char) << 2;
            next();
            break;
        case Struct:
        case Union:
            atk = tk;
            next();
            if (tk == Id) {
                if (!id->type)
                    id->type = tnew++ << 2;
                bt = id->type;
                next();
            } else {
                bt = tnew++ << 2;
            }
            if (tk == '{') {
                next();
                if (members[bt >> 2])
                    fatal("duplicate structure definition");
                tsize[bt >> 2] = 0; // for unions
                i = 0;
                while (tk != '}') {
                    int mbt = INT; // Enum
                    switch (tk) {
                    case Char:
                    case Int:
                    case Float:
                        mbt = (tk - Char) << 2;
                        next();
                        break;
                    case Struct:
                    case Union:
                        next();
                        if (tk != Id || id->type <= ATOM_TYPE || id->type >= PTR)
                            fatal("bad struct/union declaration");
                        mbt = id->type;
                        next();
                        break;
                    }
                    while (tk != ';') {
                        ty = mbt;
                        // if the beginning of * is a pointer type,
                        // then type plus `PTR` indicates what kind of pointer
                        while (tk == Mul) {
                            next();
                            ty += PTR;
                        }
                        if (tk != Id)
                            fatal("bad struct member definition");
                        sz = (ty >= PTR) ? sizeof(int) : tsize[ty >> 2];
                        struct member_s* m = sys_malloc(sizeof(struct member_s), 1);
                        m->id = id;
                        m->etype = 0;
                        next();
                        if (tk == Bracket) {
                            j = ty;
                            loc_array_decl(0, nd, &nf, &m->etype, &sz);
                            ty = (j + PTR) | nf;
                        }
                        sz = (sz + 3) & -4;
                        m->offset = i;
                        m->type = ty;
                        m->next = members[bt >> 2];
                        members[bt >> 2] = m;
                        i += sz;
                        if (atk == Union) {
                            if (i > tsize[bt >> 2])
                                tsize[bt >> 2] = i;
                            i = 0;
                        }
                        if (tk == ',')
                            next();
                    }
                    next();
                }
                next();
                if (atk != Union)
                    tsize[bt >> 2] = i;
            }
            break;
        }
        /* parse statement such as 'int a, b, c;'
         * "enum" finishes by "tk == ';'", so the code below will be skipped.
         * While current token is not statement end or block end.
         */
        b = 0;
        while (tk != ';' && tk != '}' && tk != ',' && tk != ')') {
            ty = bt;
            // if the beginning of * is a pointer type, then type plus `PTR`
            // indicates what kind of pointer
            while (tk == Mul) {
                next();
                ty += PTR;
            }
            switch (ctx) { // check non-callable identifiers
            case Glo:
                if (tk != Id)
                    fatal("bad global declaration");
                if (id->class >= ctx)
                    fatal("duplicate global definition");
                break;
            case Loc:
                if (tk != Id)
                    fatal("bad local declaration");
                if (id->class >= ctx)
                    fatal("duplicate local definition");
                break;
            }
            next();
            if (tk == '(') {
                rtf = 0;
                rtt = (ty == 0 && !memcmp(dd->name, "void", 4)) ? -1 : ty;
            }
            dd = id;
            if (dd->forward && (dd->type != ty))
                fatal("Function return type does not match prototype");
            dd->type = ty;
            if (tk == '(') { // function
                if (b != 0)
                    fatal("func decl can't be mixed with var decl(s)");
                if (ctx != Glo)
                    fatal("nested function");
                if (ty > ATOM_TYPE && ty < PTR)
                    fatal("return type can't be struct");
                if (id->class == Func && id->val > (int)text_base && id->val < (int)e &&
                    id->forward == 0)
                    fatal("duplicate global definition");
                int ddetype = 0;
                dd->class = Func;       // type is function
                dd->val = (int)(e + 1); // function Pointer? offset/address
                next();
                nf = ir_count = ld = 0; // "ld" is parameter's index.
                while (tk != ')') {
                    stmt(Par);
                    ddetype = ddetype * 2;
                    if (ty == FLOAT) {
                        ++nf;
                        ++ddetype;
                    }
                    if (tk == ',')
                        next();
                }
                if (ld > ADJ_MASK)
                    fatal("maximum of %d function parameters", ADJ_MASK);
                // function etype is not like other etypes
                next();
                ddetype = (ddetype << 10) + (nf << 5) + ld; // prm info
                if (dd->forward && (ddetype != dd->etype))
                    fatal("parameters don't match prototype");
                if (dd->forward) { // patch the forward jump
                    *(dd->forward) = dd->val;
                    dd->forward = 0;
                }
                dd->etype = ddetype;
                uint16_t* se;
                if (tk == ';') { // check for prototype
                    se = e;
                    emit_JMP((int)e);
                    dd->forward = e;
                } else { // function with body
                    if (tk != '{')
                        fatal("bad function definition");
                    loc = ++ld;
                    next();
                    // Not declaration and must not be function, analyze inner block.
                    // e represents the address which will store pc
                    // (ld - loc) indicates memory size to allocate
                    ast_Single(';');
                    while (tk != '}') {
                        int* t = n;
                        check_label(&t);
                        stmt(Loc);
                        if (t != n)
                            ast_Begin((int)t);
                    }
                    if (rtf == 0 && rtt != -1)
                        fatal("expecting return value");
                    ast_Enter(ld - loc);
                    cas = 0;
                    se = e;
                    gen(n);
                    patch_literals();
                }
                if (src_opt) {
                    printf("%d: %.*s\n", line, p - lp, lp);
                    lp = p;
                    disasm_address(&state, (int)(se + 1));
                    while (state.address < (int)(e - 1)) {
                        uint16_t* nxt = (uint16_t*)(state.address + state.size);
                        disasm_thumb(&state, *nxt, *(nxt + 1));
                        printf("%s\n", state.text);
                    }
                }
                id = sym;
                if (src_opt)
                    memset(ir_var, 0, sizeof(struct ident_s*) * MAX_IR);
                while (id->tk) { // unwind symbol table locals
                    if (id->class == Loc || id->class == Par) {
                        id->class = id->hclass;
                        id->type = id->htype;
                        id->val = id->hval;
                        id->etype = id->hetype;
                    } else if (id->class == Label) { // clear id for next func
                        id->class = 0;
                        id->val = 0;
                        id->type = 0;
                    } else if (id->class == 0 && id->type == -1)
                        fatal("%d: label %.*s not defined\n", line, id->hash & 0x3f, id->name);
                    id++;
                }
            } else {
                dd->hclass = dd->class;
                dd->class = ctx;
                dd->htype = dd->type;
                dd->type = ty;
                dd->hval = dd->val;
                dd->hetype = dd->etype;
                sz = (ty >= PTR) ? sizeof(int) : tsize[ty >> 2];
                if (tk == Bracket) {
                    i = ty;
                    loc_array_decl(ctx, nd, &j, &dd->etype, &sz);
                    ty = (i + PTR) | j;
                    dd->type = ty;
                }
                sz = (sz + 3) & -4;
                if (ctx == Glo) {
                    dd->val = (int)data;
                    if (data + sz >= data_base + (DATA_BYTES / 4))
                        fatal("program data exceeds data segment");
                    data += sz;
                } else if (ctx == Loc) {
                    dd->val = (ld += sz / sizeof(int));
                    ir_var[ir_count++] = dd;
                } else if (ctx == Par) {
                    if (ty > ATOM_TYPE && ty < PTR) // local struct decl
                        fatal("struct parameters must be pointers");
                    dd->val = ld++;
                    ir_var[ir_count++] = dd;
                }
                if (tk == Assign) {
                    next();
                    if (ctx == Par)
                        fatal("default arguments not supported");
                    if (tk == '{' && (dd->type & 3))
                        init_array(dd, nd, j);
                    else {
                        if (ctx == Loc) {
                            if (b == 0)
                                ast_Single(';');
                            b = n;
                            ast_Loc(loc - dd->val);
                            a = n;
                            i = ty;
                            expr(Assign);
                            typecheck(Assign, i, ty);
                            ast_Assign((int)a, (ty << 16) | i);
                            ty = i;
                            ast_Begin((int)b);
                        } else { // ctx == Glo
                            i = ty;
                            expr(Cond);
                            typecheck(Assign, i, ty);
                            if (ast_Tk(n) != Num && ast_Tk(n) != NumF)
                                fatal("global assignment must eval to lit expr");
                            if (ty == CHAR + PTR && (dd->type & 3) != 1)
                                fatal("use decl char foo[nn] = \"...\";");
                            if ((ast_Tk(n) == Num && (i == CHAR || i == INT)) ||
                                (ast_Tk(n) == NumF && i == FLOAT))
                                *((int*)dd->val) = ast_NumVal(n);
                            else if (ty == CHAR + PTR) {
                                i = strlen((char*)ast_NumVal(n)) + 1;
                                if (i > (dd->etype + 1)) {
                                    i = dd->etype + 1;
                                    printf("%d: string truncated to width\n", line);
                                }
                                memcpy((char*)dd->val, (char*)ast_NumVal(n), i);
                            } else
                                fatal("unsupported global initializer");
                            n += 2;
                        }
                    }
                }
            }
            if (ctx != Par && tk == ',')
                next();
        }
        return;
    case If:
        next();
        if (tk != '(')
            fatal("open parenthesis expected");
        next();
        expr(Assign);
        a = n;
        if (tk != ')')
            fatal("close parenthesis expected");
        next();
        stmt(ctx);
        b = n;
        if (tk == Else) {
            next();
            stmt(ctx);
            d = n;
        } else
            d = 0;
        ast_Cond((int)d, (int)b, (int)a);
        return;
    case While:
        next();
        if (tk != '(')
            fatal("open parenthesis expected");
        next();
        expr(Assign);
        b = n; // condition
        if (tk != ')')
            fatal("close parenthesis expected");
        next();
        ++brkc;
        ++cntc;
        stmt(ctx);
        a = n; // parse body of "while"
        --brkc;
        --cntc;
        ast_While((int)b, (int)a, While);
        return;
    case DoWhile:
        next();
        ++brkc;
        ++cntc;
        stmt(ctx);
        a = n; // parse body of "do-while"
        --brkc;
        --cntc;
        if (tk != While)
            fatal("while expected");
        next();
        if (tk != '(')
            fatal("open parenthesis expected");
        next();
        ast_Single(';');
        expr(Assign);
        b = n;
        if (tk != ')')
            fatal("close parenthesis expected");
        next();
        ast_While((int)b, (int)a, DoWhile);
        return;
    case Switch:
        i = 0;
        j = 0;
        if (cas)
            j = (int)cas;
        cas = &i;
        next();
        if (tk != '(')
            fatal("open parenthesis expected");
        next();
        expr(Assign);
        a = n;
        if (tk != ')')
            fatal("close parenthesis expected");
        next();
        ++swtc;
        ++brkc;
        stmt(ctx);
        --swtc;
        --brkc;
        b = n;
        ast_Switch((int)b, (int)a);
        if (j)
            cas = (int*)j;
        return;
    case Case:
        if (!swtc)
            fatal("case-statement outside of switch");
        i = *cas;
        next();
        expr(Or);
        a = n;
        if (ast_Tk(n) != Num)
            fatal("case label not a numeric literal");
        j = ast_NumVal(n);
        ast_NumVal(n) -= i;
        *cas = j;
        ast_Single(';');
        if (tk != ':')
            fatal("colon expected");
        next();
        stmt(ctx);
        b = n;
        ast_Case((int)b, (int)a);
        return;
    case Break:
        if (!brkc)
            fatal("misplaced break statement");
        next();
        if (tk != ';')
            fatal("semicolon expected");
        next();
        ast_Single(Break);
        return;
    case Continue:
        if (!cntc)
            fatal("misplaced continue statement");
        next();
        if (tk != ';')
            fatal("semicolon expected");
        next();
        ast_Single(Continue);
        return;
    case Default:
        if (!swtc)
            fatal("default-statement outside of switch");
        next();
        if (tk != ':')
            fatal("colon expected");
        next();
        stmt(ctx);
        a = n;
        ast_Default((int)a);
        return;
    // RETURN_stmt -> 'return' expr ';' | 'return' ';'
    case Return:
        a = 0;
        next();
        if (tk != ';') {
            expr(Assign);
            a = n;
            if (rtt == -1)
                fatal("not expecting return value");
            typecheck(Eq, rtt, ty);
        } else {
            if (rtt != -1)
                fatal("return value expected");
        }
        rtf = 1; // signal a return statement exisits
        ast_Return((int)a);
        if (tk != ';')
            fatal("semicolon expected");
        next();
        return;
    /* For iteration is implemented as:
     * Init -> Cond -> Bz to end -> Jmp to Body
     * After -> Jmp to Cond -> Body -> Jmp to After
     */
    case For:
        next();
        if (tk != '(')
            fatal("open parenthesis expected");
        next();
        ast_Single(';');
        if (tk != ';')
            expr(Assign);
        while (tk == ',') {
            int* f = n;
            next();
            expr(Assign);
            ast_Begin((int)f);
        }
        d = n;
        if (tk != ';')
            fatal("semicolon expected");
        next();
        ast_Single(';');
        if (tk != ';') {
            expr(Assign);
            a = n; // Point to entry of for cond
            if (tk != ';')
                fatal("semicolon expected");
        } else
            a = 0;
        next();
        ast_Single(';');
        if (tk != ')')
            expr(Assign);
        while (tk == ',') {
            int* g = n;
            next();
            expr(Assign);
            ast_Begin((int)g);
        }
        b = n;
        if (tk != ')')
            fatal("close parenthesis expected");
        next();
        ++brkc;
        ++cntc;
        stmt(ctx);
        c = n;
        --brkc;
        --cntc;
        ast_For((int)d, (int)c, (int)b, (int)a);
        return;
    case Goto:
        next();
        if (tk != Id || (id->type != 0 && id->type != -1) || (id->class != Label && id->class != 0))
            fatal("goto expects label");
        id->type = -1; // hack for id->class deficiency
        ast_Goto((int)id);
        next();
        if (tk != ';')
            fatal("semicolon expected");
        next();
        return;
    // stmt -> '{' stmt '}'
    case '{':
        next();
        ast_Single(';');
        while (tk != '}') {
            a = n;
            check_label(&a);
            stmt(ctx);
            if (a != n)
                ast_Begin((int)a);
        }
        next();
        return;
    // stmt -> ';'
    case ';':
        next();
        ast_Single(';');
        return;
    default:
        expr(Assign);
        if (tk != ';' && tk != ',')
            fatal("semicolon expected");
        next();
    }
}

static Inline float i_as_f(int i) {
    union {
        int i;
        float f;
    } u;
    u.i = i;
    return u.f;
}

static Inline int f_as_i(float f) {
    union {
        int i;
        float f;
    } u;
    u.f = f;
    return u.i;
}

static int common_vfunc(int ac, int sflag, int* sp) {
    // HACK ALLERT, we need to figure out which parameters
    // are floats. Scan the format string.
    int stack[ADJ_MASK + ADJ_MASK + 2];
    int stkp = 0;
    int n_parms = (ac & ADJ_MASK);
    ac >>= 10;
    for (int j = n_parms - 1; j >= 0; j--)
        if ((ac & (1 << j)) == 0)
            stack[stkp++] = sp[j];
        else {
            if (stkp & 1)
                stack[stkp++] = 0;
            union {
                double d;
                int ii[2];
            } u;
            u.d = *((float*)&sp[j]);
            stack[stkp++] = u.ii[0];
            stack[stkp++] = u.ii[1];
        }
    int r = cc_printf(stack, stkp, sflag);
    if (!sflag)
        fflush(stdout);
    return r;
}

static void show_defines(struct define_grp* grp) {
    if (grp->name == 0)
        return;
    printf("\nPredefined symbols:\n\n");
    int x, y;
    get_screen_xy(&x, &y);
    int pos = 0;
    for (; grp->name; grp++) {
        if (pos == 0) {
            pos = strlen(grp->name);
            printf("%s", grp->name);
        } else {
            if (pos + strlen(grp->name) + 2 > x) {
                pos = strlen(grp->name);
                printf("\n%s", grp->name);
            } else {
                pos += strlen(grp->name) + 2;
                printf(", %s", grp->name);
            }
        }
    }
    if (pos)
        printf("\n");
}

static void show_externals(int i) {
    printf("\nFunctions:\n\n");
    int x, y;
    get_screen_xy(&x, &y);
    int pos = 0;
    for (int j = 0; j < numof(externs); j++)
        if (externs[j].grp == includes[i].grp) {
            if (pos == 0) {
                pos = strlen(externs[j].name);
                printf("%s", externs[j].name);
            } else {
                if (pos + strlen(externs[j].name) + 2 > x) {
                    pos = strlen(externs[j].name);
                    printf("\n%s", externs[j].name);
                } else {
                    pos += strlen(externs[j].name) + 2;
                    printf(", %s", externs[j].name);
                }
            }
        }
    if (pos)
        printf("\n");
}

static void help(char* lib) {
    if (!lib) {
        printf("\n"
               "usage: cc [-s] [-t[i]] [-h [lib]] [-D [symbol[ = value]]] [-o filename] filename\n"
               "    -s      display disassembly and quit.\n"
               "    -t,-ti  trace execution. i enables single step.\n"
               "    -D symbol [= value]\n"
               "            define symbol for limited pre-processor.\n"
               "    -h      Compiler help. lib lists externals.\n"
               "    filename\n"
               "            C source file name.\n"
               "Libraries:\n"
               "    %s",
               includes[0]);
        for (int i = 1; includes[i].name; i++) {
            printf(", %s", includes[i].name);
            if ((i % 8) == 0 && includes[i + 1].name)
                printf("\n    %s", includes[++i].name);
        }
        printf("\n");
        return;
    }
    for (int i = 0; includes[i].name; i++)
        if (!strcmp(lib, includes[i].name)) {
            show_externals(i);
            if (includes[i].grp)
                show_defines(includes[i].grp);
            return;
        }
    fatal("unknown lib %s", lib);
    return;
}

static void add_defines(struct define_grp* d) {
    for (; d->name; d++) {
        p = d->name;
        next();
        id->class = Num;
        id->type = INT;
        id->val = d->val;
    }
}

int cc(int argc, char** argv) {

    clear_globals();

    if (setjmp(done_jmp))
        goto done;

    sym = sys_malloc(SYM_TBL_BYTES, 1);

    // Register keywords in symbol stack. Must match the sequence of enum
    p = "enum char int float struct union sizeof return goto break continue "
        "if do while for switch case default else void main";

    // call "next" to create symbol table entry.
    // store the keyword's token type in the symbol table entry's "tk" field.
    for (int i = Enum; i <= Else; ++i) {
        next();
        id->tk = i;
        id->class = Keyword; // add keywords to symbol table
    }

    next();

    id->tk = Char;
    id->class = Keyword; // handle void type
    next();
    struct ident_s* idmain = id;
    id->class = Main; // keep track of main

    data_base = data = sys_malloc(DATA_BYTES, 1);
    tsize = sys_malloc(TS_TBL_BYTES, 1);
    ast = sys_malloc(AST_TBL_BYTES, 1);
    n = ast + (AST_TBL_BYTES / 4) - 1;

    // add primitive types
    tsize[tnew++] = sizeof(char);
    tsize[tnew++] = sizeof(int);
    tsize[tnew++] = sizeof(float);
    tsize[tnew++] = 0; // reserved for another scalar type

    --argc;
    ++argv;
    char* lib_name = NULL;
    while (argc > 0 && **argv == '-') {
        if ((*argv)[1] == 'h') {
            --argc;
            ++argv;
            if (argc)
                lib_name = *argv;
            help(lib_name);
            goto done;
        } else if ((*argv)[1] == 's') {
            src_opt = 1;
        } else if ((*argv)[1] == 't') {
            trc_opt = ((*argv)[2] == 'i') ? 2 : 1;
        } else if ((*argv)[1] == 'D') {
            p = &(*argv)[2];
            next();
            if (tk != Id)
                fatal("bad -D identifier");
            struct ident_s* dd = id;
            next();
            int i = 0;
            if (tk == Assign) {
                next();
                expr(Cond);
                if (ast_Tk(n) != Num)
                    fatal("bad -D initializer");
                i = ast_NumVal(n);
                n += 2;
            }
            dd->class = Num;
            dd->type = INT;
            dd->val = i;
        } else
            argc = 0; // bad compiler option. Force exit.
        --argc;
        ++argv;
    }
    if (argc < 1) {
        help(NULL);
        goto done;
    }

    if (src_opt)
        disasm_init(&state, DISASM_ADDRESS | DISASM_INSTR | DISASM_COMMENT);
    add_defines(stdio_defines);
    add_defines(gpio_defines);
    add_defines(pwm_defines);
    add_defines(clk_defines);
    add_defines(i2c_defines);
    add_defines(spi_defines);
    add_defines(irq_defines);

    char* fn = sys_malloc(strlen(full_path(*argv)) + 3, 1);
    strcpy(fn, full_path(*argv));
    if (strrchr(fn, '.') == NULL)
        strcat(fn, ".c");
    fd = sys_malloc(sizeof(lfs_file_t), 1);
    if (fs_file_open(fd, fn, LFS_O_RDONLY) < LFS_ERR_OK) {
        sys_free(fd);
        fd = NULL;
        fatal("could not open %s \n", fn);
    }
    sys_free(fn);

    int siz = fs_file_seek(fd, 0, LFS_SEEK_END);
    fs_file_rewind(fd);

    text_base = le = sys_malloc(TEXT_BYTES, 1);
    e = text_base - 1;
    members = sys_malloc(MEMBER_DICT_BYTES, 1);

    src = lp = p = sys_malloc(siz + 1, 1);
    if (fs_file_read(fd, p, siz) < LFS_ERR_OK)
        fatal("unable to read from source file");
    p[siz] = 0;
    fs_file_close(fd);
    sys_free(fd);
    fd = NULL;

    // parse the program
    line = 1;
    pplevt = -1;
    next();
    while (tk) {
        stmt(Glo);
        next();
    }
    // check for unpatched forward JMPs
    for (struct ident_s* scan = sym; scan->tk; ++scan)
        if (scan->class == Func && scan->forward)
            fatal("undeclared forward function %.*s", scan->hash & 0x3f, scan->name);
    sys_free(ast);
    ast = NULL;
    sys_free(src);
    src = NULL;
    sys_free(sym);
    sym = NULL;
    sys_free(tsize);
    tsize = NULL;
    if (!idmain->val)
        fatal("main() not defined\n");

    if (src_opt)
        goto done;

    printf("\n");

done:
    test_func();
    if (src_opt)
        disasm_cleanup(&state);
    if (fd)
        fs_file_close(fd);
    while (file_list) {
        if (file_list->is_dir)
            fs_dir_close(&file_list->u.dir);
        else
            fs_file_close(&file_list->u.file);
        file_list = file_list->next;
    }
    while (malloc_list) {
        // printf("%08x %d\n", (int)malloc_list, *((int*)malloc_list + 1));
        sys_free(malloc_list + 2);
    }

    return 0;
}
