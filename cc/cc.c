/*
 * mc is capable of compiling a (subset of) C source files
 * There is no preprocessor.
 *
 * See C4 and AMaCC project repositories for baseline code.
 * float, array, struct support in squint project by HPCguy.
 * native code generation for RP Pico by lurk101.
 *
 */

#include "cc.h"

// pshell integration
#include "../pshell/main.h"
#include "../pshell/terminal.h"

// clib functions
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// pico SDK hardware support functions
#include <hardware/adc.h>
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/i2c.h>
#include <hardware/irq.h>
#include <hardware/pwm.h>
#include <hardware/spi.h>
#include <hardware/sync.h>

// pico SDK accellerated functions
#include <pico/rand.h>
#include <pico/stdio.h>
#include <pico/time.h>

// disassembler, compiler, and file system functions
#include "armdisasm.h"
#include "io.h"

#include "cc_internals.h"
#include "cc_malloc.h"
#include "cc_wraps.h"
#include "cc_ast.h"
#include "cc_parse.h"
#include "cc_peep.h"
#include "cc_gen.h"

extern void cc_exit(int rc);                         // C exit function
extern char __StackLimit[TEXT_BYTES + DATA_BYTES];   // start of code segment

void (*fops[])() = { //
    0,
    __wrap___aeabi_idiv,
    __wrap___aeabi_i2f,
    __wrap___aeabi_f2iz,
    __wrap___aeabi_fadd,
    __wrap___aeabi_fsub,
    __wrap___aeabi_fmul,
    __wrap___aeabi_fdiv,
    __wrap___aeabi_fcmple,
    __wrap___aeabi_fcmpgt,
    __wrap___aeabi_fcmplt,
    __wrap___aeabi_fcmpge
};

struct reloc_s* relocs UDATA; // relocation list root
int nrelocs UDATA;            // relocation list size

char *p UDATA, *lp UDATA;                       // current position in source code
char* data UDATA;                               // data/bss pointer
char* data_base UDATA;                          // data/bss pointer
int* base_sp UDATA;                             // stack
uint16_t *e UDATA, *le UDATA, *text_base UDATA; // current position in emitted code
uint16_t* ecas UDATA;                           // case statement patch-up pointer
int* ncas UDATA;                                // case statement patch-up pointer
uint16_t* def UDATA;                            // default statement patch-up pointer
struct patch_s* brks UDATA;                     // break statement patch-up pointer
struct patch_s* cnts UDATA;                     // continue statement patch-up pointer
struct patch_s* pcrel UDATA;                    // pc relative address patch-up pointer
uint16_t* pcrel_1st UDATA;                      // first relative load address in group
int pcrel_count UDATA;                          // first relative load address in group
int swtc UDATA;                                 // !0 -> in a switch-stmt context
int brkc UDATA;                                 // !0 -> in a break-stmt context
int cntc UDATA;                                 // !0 -> in a continue-stmt context
int* tsize UDATA;                               // array (indexed by type) of type sizes
int tnew UDATA;                                 // next available type
int tk UDATA;                                   // current token
union conv tkv UDATA;                           // current token value
int ty UDATA;                                   // current expression type
                                                       // bit 0:1 - tensor rank, eg a[4][4][4]
                                                       // 0=scalar, 1=1d, 2=2d, 3=3d
                                                       //   1d etype -- bit 0:30)
                                                       //   2d etype -- bit 0:15,16:30 [32768,65536]
//   3d etype -- bit 0:10,11:20,21:30 [1024,1024,2048]
// bit 2:9 - type
// bit 10:11 - ptr level
int compound UDATA;            // manage precedence of compound assign expressions
int rtf UDATA, rtt UDATA;      // return flag and return type for current function
int loc UDATA;                 // local variable offset
int lineno UDATA;              // current line number
int src_opt UDATA;             // print source and assembly flag
int nopeep_opt UDATA;          // turn off peep-hole optimization
int uchar_opt UDATA;           // use unsigned character variables
int* n UDATA;                         // current position in emitted abstract syntax tree
                                      // With an AST, the compiler is not limited to generate
                                      // code on the fly with parsing.
                                      // This capability allows function parameter code to be
                                      // emitted and pushed on the stack in the proper
                                      // right-to-left order.
int ld UDATA;                  // local variable depth
int pplev UDATA, pplevt UDATA; // preprocessor conditional level
int* ast UDATA;                       // abstract syntax tree
ARMSTATE state UDATA;          // disassembler state
int exit_sp UDATA;                    // stack at entry to main
char* ofn UDATA;               // output file (executable) name
int indef UDATA;               // parsing in define statement
char* src_base UDATA;          // source code region

// symbol table
struct ident_s* id UDATA;       // currently parsed identifier
struct ident_s* sym_base UDATA; // symbol table (simple list of identifiers)

struct member_s** members UDATA; // array (indexed by type) of struct member lists

struct file_handle* file_list UDATA; // file list root

// tokens and classes (operators last and in precedence order)
// ( >= 128 so not to collide with ASCII-valued tokens)
#include "cc_tokns.h"

// operations
#include "cc_ops.h"

// library help for external functions

struct define_grp {
    const char* name; // function group name
    const int val;    // index of 1st function in group
};

// predefined external functions
#include "cc_defs.h"

static jmp_buf done_jmp UDATA; // fatal error jump address

__attribute__((__noreturn__))
void fatal_func(const char* func, int lne, const char* fmt, ...) {
    printf("\n");
#ifndef NDEBUG
    printf("error in compiler function %s at line %d\n", func, lne);
#endif
    printf(VT_BOLD "Error : " VT_NORMAL);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (lineno > 0) {
        lp = src_base;
        int lno = lineno;
        while (--lno) {
            lp = strchr(lp, '\n') + 1;
        }
        p = strchr(lp, '\n');
        printf("\n" VT_BOLD "%d:" VT_NORMAL " %.*s\n", lineno, p - lp, lp);
    }
    longjmp(done_jmp, 1); // bail out
}

__attribute__((__noreturn__))
void run_fatal(const char* fmt, ...) {
    printf("\n" VT_BOLD "run time error : " VT_NORMAL);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    longjmp(done_jmp, 1); // bail out
}

#define numof(a) (sizeof(a) / sizeof(a[0]))

const struct help_grp includes[] = {
    {"stdio", stdio_defines},   {"stdlib", stdlib_defines},
    {"string", string_defines}, {"math", math_defines},
    {"sync", sync_defines},     {"time", time_defines},
    {"gpio", gpio_defines},     {"pwm", pwm_defines},
    {"adc", adc_defines},       {"clocks", clk_defines},
    {"i2c", i2c_defines},       {"spi", spi_defines},
    {"irq", irq_defines},       {0}
};

static lfs_file_t* fd UDATA;
static char* fp UDATA;

// Help display

static void show_defines(const struct define_grp* grp) {
    if (grp->name == 0) {
        return;
    }
    printf("Predefined symbols:\n\n");
    int x = term_cols;
    int y = term_rows;
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
    if (pos) {
        printf("\n");
    }
}

// #include "cc_defs.h"

const struct externs_s externs[] = {
#include "cc_extrns.h"
};

// numof(externs) will fail in any file outside of where externs is defined,
// due to "invalid application of 'sizeof' to incomplete type 'const struct externs_s[]'"
size_t numof_externs() {
    return numof(externs);
}

static void show_externals(int i) {
    printf("Functions:\n\n");
    int x = term_cols;
    int y = term_rows;
    int pos = 0;
    for (int j = 0; j < numof(externs); j++) {
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
    }
    if (pos) {
        printf("\n");
    }
}

static void help(char* lib) {
    if (!lib) {
        printf("Usage: cc [-s] [-u] [-n] [-h [lib]] [-Dsymbol[=integer]]\n"
               "          [-o exename] filename.c\n"
               "  -s      display disassembly and quit.\n"
               "  -o      name of executable output file.\n"
               "  -u      treat char type as unsigned.\n"
               "  -n      turn off peep-hole optimization\n"
               "  -Dsymbol[=integer]\n"
               "          define symbol for limited pre-processor.\n"
               "  -h      show compiler help and list libraries.\n"
               "  -h lib  show available functions and symbols from <lib>.\n"
               "  filename.c\n"
               "          C source file name.\n"
               "\n"
               "Examples:\n"
               "  cc hello.c\n"
               "  cc -DFOO -DBAR=42 hello.c\n"
               "  cc -h\n"
               "  cc -h math\n"
               "\n"
               "Libraries:\n"
               "  %s",
               includes[0]);
        for (int i = 1; includes[i].name; i++) {
            printf(", %s", includes[i].name);
            if ((i % 8) == 0 && includes[i + 1].name) {
                printf("\n  %s", includes[++i].name);
            }
        }
        printf("\n");
        return;
    }
    for (int i = 0; includes[i].name; i++) {
        if (!strcmp(lib, includes[i].name)) {
            show_externals(i);
            printf("\n");
            if (includes[i].grp) {
                show_defines(includes[i].grp);
            }
            return;
        }
    }
    fatal("unknown lib %s", lib);
    return;
}

static void add_defines(const struct define_grp* d) {
    for (; d->name; d++) {
        p = (char*)d->name;
        next();
        id->class = Num;
        id->type = INT;
        id->val = d->val;
    }
}

#if EXE_DBG
void __not_in_flash_func(dummy)(void) {
    asm volatile(
#include "cc_nops.h"
    );
}
#endif

// executable file header
struct exe_s {
    int entry;  // entry point
    int tsize;  // text segment size
    int dsize;  // data segment size
    int nreloc; // # of external function relocation entries
};

// compiler can be invoked in compile mode (mode = 0)
// or loader mode (mode = 1)
int cc(int mode, int argc, char** argv) {

    // clear uninitialized global variables
    extern char __ccudata_start__, __ccudata_end__;
    memset(&__ccudata_start__, 0, &__ccudata_end__ - &__ccudata_start__);

    extern const char* pshell_version;
    int rslt = -1;
    struct exe_s exe;

    // set the abort jump
    if (setjmp(done_jmp)) {
        goto done;
    }

    // compile mode
    if (mode == 0) {
        // Register keywords in symbol table. Must match the sequence of enum
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

        // add the void type symbol
        id->tk = Char;
        id->class = Keyword; // handle void type
        next();

        // add the main symbol
        struct ident_s* idmain = id;
        id->class = Main; // keep track of main

        // set data segment bases
        data_base = data = __StackLimit + TEXT_BYTES;
        memset(__StackLimit, 0, TEXT_BYTES + DATA_BYTES);
        // allocate the type size and abstract syntax tree
        tsize = cc_malloc(TS_TBL_BYTES, 1);
        ast = cc_malloc(AST_TBL_BYTES, 1);
        n = ast + (AST_TBL_BYTES / 4) - 1;

        // add primitive type sizes
        tsize[tnew++] = sizeof(char);
        tsize[tnew++] = sizeof(int);
        tsize[tnew++] = sizeof(float);
        tsize[tnew++] = 0; // reserved for another scalar type

        // parse the command line arguments
        --argc;
        ++argv;
        char* lib_name = NULL;
        while (argc > 0 && **argv == '-') {
            if ((*argv)[1] == 'h') {
                --argc;
                ++argv;
                if (argc) {
                    lib_name = *argv;
                }
                help(lib_name);
                goto done;
            } else if ((*argv)[1] == 's') {
                src_opt = 1;
            } else if ((*argv)[1] == 'n') {
                nopeep_opt = 1;
            } else if ((*argv)[1] == 'o') {
                --argc;
                ++argv;
                if (argc) {
                    ofn = *argv;
                }
            } else if ((*argv)[1] == 'u') {
                uchar_opt = 1;
            } else if ((*argv)[1] == 'D') {
                p = &(*argv)[2];
                next();
                if (tk != Id) {
                    fatal("bad -D identifier");
                }
                struct ident_s* dd = id;
                next();
                int i = 0;
                if (tk == Assign) {
                    next();
                    expr(Cond);
                    if (ast_Tk(n) != Num) {
                        fatal("bad -D initializer: must be an integer");
                    }
                    i = Num_entry(n).val;
                    n += Num_words;
                }
                dd->class = Num;
                dd->type = INT;
                dd->val = i;
            } else {
                argc = 0; // bad compiler option. Force exit.
            }
            --argc;
            ++argv;
        }
        // input file name is mandatory parameter
        if (argc < 1) {
            help(NULL);
            goto done;
        }

        // optionally enable and add known symbols to disassembler tables
        if (src_opt) {
            disasm_init(&state, DISASM_ADDRESS | DISASM_INSTR | DISASM_COMMENT);
            disasm_symbol(&state, "idiv", (uint32_t)__wrap___aeabi_idiv, ARMMODE_THUMB);
            disasm_symbol(&state, "i2f", (uint32_t)__wrap___aeabi_i2f, ARMMODE_THUMB);
            disasm_symbol(&state, "f2i", (uint32_t)__wrap___aeabi_f2iz, ARMMODE_THUMB);
            disasm_symbol(&state, "fadd", (uint32_t)__wrap___aeabi_fadd, ARMMODE_THUMB);
            disasm_symbol(&state, "fsub", (uint32_t)__wrap___aeabi_fsub, ARMMODE_THUMB);
            disasm_symbol(&state, "fmul", (uint32_t)__wrap___aeabi_fmul, ARMMODE_THUMB);
            disasm_symbol(&state, "fdiv", (uint32_t)__wrap___aeabi_fdiv, ARMMODE_THUMB);
            disasm_symbol(&state, "fcmple", (uint32_t)__wrap___aeabi_fcmple, ARMMODE_THUMB);
            disasm_symbol(&state, "fcmpge", (uint32_t)__wrap___aeabi_fcmpge, ARMMODE_THUMB);
            disasm_symbol(&state, "fcmpgt", (uint32_t)__wrap___aeabi_fcmpgt, ARMMODE_THUMB);
            disasm_symbol(&state, "fcmplt", (uint32_t)__wrap___aeabi_fcmplt, ARMMODE_THUMB);
        }

        // add SDK and clib symbols
        add_defines(stdio_defines);
        add_defines(gpio_defines);
        add_defines(pwm_defines);
        add_defines(clk_defines);
        add_defines(i2c_defines);
        add_defines(spi_defines);
        add_defines(irq_defines);

        // make a copy of the full path and append .c if necessary
        char* fn = cc_malloc(strlen(full_path(*argv)) + 3, 1);
        strcpy(fn, full_path(*argv));
        // allocate a file descriptor and open the input file
        fd = cc_malloc(sizeof(lfs_file_t), 1);
        if (fs_file_open(fd, fn, LFS_O_RDONLY) < LFS_ERR_OK) {
            cc_free(fd);
            fd = NULL;
            fatal("could not open %s \n", fn);
        }
        // don't need the filename anymore
        cc_free(fn);
        // get the file size
        int fl = fs_file_seek(fd, 0, SEEK_END);
        fs_file_seek(fd, 0, SEEK_SET);
        // allocate the source buffer, terminate it, then close the file
        src_base = p = lp = cc_malloc(fl + 1, 1);
        // move the file contents to the source buffer
        if (fs_file_read(fd, src_base, fl) != fl) {
            fatal("error reading source");
        }
        src_base[fl] = 0;
        fs_file_close(fd);
        // free the file descriptor
        cc_free(fd);
        fd = 0;

        // set the code base
#if EXE_DBG
        text_base = le = (uint16_t*)((int)dummy & ~1);
#else
        text_base = le = (uint16_t*)__StackLimit;
#endif
        e = text_base - 1;

        // allocate the structure member table
        members = cc_malloc(MEMBER_DICT_BYTES, 1);

        // compile the program
        lineno = 1;
        pplevt = -1;
        next();
        while (tk) {
            stmt(Glo);
            next();
        }
        // check for undeclared forward functions
        for (id = sym_base; id; id = id->next) {
            if (id->class == Func && id->forward) {
                fatal("undeclared forward function %.*s", id->hash & 0x3f, id->name);
            }
        }

        // free all the compiler buffers
        cc_free_all();
        src_base = NULL;
        ast = NULL;
        sym_base = NULL;
        tsize = NULL;

        if (src_opt) {
            disasm_cleanup(&state);
        }

        // entry point main must be declared
        if (!idmain->val) {
            fatal("main() not defined\n");
        }

        // save the entry point address
        exe.entry = idmain->val;

        // optionally create executable output file
        if (ofn) {
            // allocate output file descriptor and create the file
            fd = cc_malloc(sizeof(lfs_file_t), 1);
            char* cp = full_path(ofn);
            if (fs_file_open(fd, cp, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) < LFS_ERR_OK) {
                cc_free(fd);
                fd = NULL;
                fatal("could not create %s\n", full_path(ofn));
            }
            // initialize the header and write it
            exe.tsize = ((e + 1) - text_base) * sizeof(*e);
            exe.dsize = (data - data_base) | 0xc0000000;
            exe.nreloc = nrelocs;
            if (fs_file_write(fd, &exe, sizeof(exe)) != sizeof(exe)) {
                fs_file_close(fd);
                fatal("error writing executable file");
            }
            // write the code segment
            if (fs_file_write(fd, text_base, exe.tsize) != exe.tsize) {
                fs_file_close(fd);
                fatal("error writing executable file");
            }
            // write the data segment
            int ds = exe.dsize & 0x3fffffff;
            if (ds && fs_file_write(fd, data_base, ds) != ds) {
                fs_file_close(fd);
                fatal("error writing executable file");
            }
            // write the external function relocation list
            while (relocs) {
                if (fs_file_write(fd, &relocs->addr, sizeof(relocs->addr)) !=
                    sizeof(relocs->addr)) {
                    fs_file_close(fd);
                    fatal("error writing executable file");
                }
                struct reloc_s* r = relocs->next;
                cc_free(relocs);
                relocs = r;
            }
            // done. close the file and set the executable attribute
            fs_file_close(fd);
            cc_free(fd);
            fd = NULL;
            if (fs_setattr(full_path(ofn), 1, "exe", 4) < LFS_ERR_OK) {
                fatal("unable to set executable attribute");
            }
            printf("\ntext  %06x\ndata  %06x\nentry %06x\nreloc %06x\n", exe.tsize, ds,
                   exe.entry - (int)text_base, exe.nreloc);
            goto done;
        }
        if (src_opt) {
            goto done;
        }

    } else { // loader mode
        // output file name is not optional
        if (argc < 1) {
            fatal("specify executable file name");
        }
        ofn = argv[0];
        // check file attribute
        char buf[4];
        if (fs_getattr(ofn, 1, buf, sizeof(buf)) != 4) {
            fatal("file %s not found or not executable", ofn);
        }
        if (memcmp(buf, "exe", 4)) {
            fatal("file %s not found or not executable", ofn);
        }
        // allocate file descriptor and open binary executable file
        fd = cc_malloc(sizeof(lfs_file_t), 1);
        if (fs_file_open(fd, ofn, LFS_O_RDONLY) < LFS_ERR_OK) {
            fatal("can't open file %s", ofn);
        }
        // read the exe header
        if (fs_file_read(fd, &exe, sizeof(exe)) != sizeof(exe)) {
            fs_file_close(fd);
            fatal("error reading %s", ofn);
        }
        if ((exe.dsize & 0xc0000000) != 0xc0000000) {
            fatal("executable compiled with earlier version not compatible, please recompile");
        }
        // clear the code segment for good measure though not necessary
        memset(__StackLimit, 0, TEXT_BYTES + DATA_BYTES);
        // read in the code segment
        if (fs_file_read(fd, __StackLimit, exe.tsize) != exe.tsize) {
            fs_file_close(fd);
            fd = NULL;
            fatal("error reading %s", ofn);
        }
        // read in the data segment
        int ds = exe.dsize & 0x3fffffff;
        if (ds && fs_file_read(fd, __StackLimit + TEXT_BYTES, ds) != ds) {
            fs_file_close(fd);
            fd = NULL;
            fatal("error reading %s", ofn);
        }
        // set all the relocatable external function calls
        for (int i = 0; i < exe.nreloc; i++) {
            int addr;
            if (fs_file_read(fd, &addr, sizeof(addr)) != sizeof(addr)) {
                fs_file_close(fd);
                fd = NULL;
                fatal("error reading %s", ofn);
            }
            int v = *((int*)addr);
            if (v < 0) {
                *((int*)addr) = (int)fops[-v];
            } else {
                if (externs[v].is_printf) {
                    *((int*)addr) = (int)x_printf;
                } else if (externs[v].is_sprintf) {
                    *((int*)addr) = (int)x_sprintf;
                } else {
                    *((int*)addr) = (int)externs[v].extrn;
                }
            }
        }
        // close the file and free its descriptor
        fs_file_close(fd);
        fd = NULL;
    }
    cc_free_all();

    // launch the user code
    printf("\n");
    asm volatile("mov  %0, sp \n" : "=r"(exit_sp));
    asm volatile("mov  r0, %2 \n"
                 "push {r0}   \n"
                 "mov  r0, %3 \n"
                 "push {r0}   \n"
                 "blx  %1     \n"
                 "add  sp, #8 \n"
                 "mov  %0, r0 \n"
                 : "=r"(rslt)
                 : "r"(exe.entry | 1), "r"(argc), "r"(argv)
                 : "r0", "r1", "r2", "r3");
    // display the return code
    printf("\nCC = %d\n", rslt);

done: // clean up and return
    if (fd) {
        fs_file_close(fd);
    }
    // unclosed files
    while (file_list) {
        if (file_list->is_dir) {
            fs_dir_close(&file_list->u.dir);
        } else {
            fs_file_close(&file_list->u.file);
        }
        file_list = file_list->next;
    }
    // unfreed memory
    cc_free_all();

    return rslt;
}
