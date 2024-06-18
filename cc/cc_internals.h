#ifndef _CC_INTERNALS_H_
#define _CC_INTERNALS_H_

#include "cc.h"
#include <stdbool.h>
#include "io.h"
#include "armdisasm.h"

// for compiler debug only,
// limited and not for normal use
#define EXE_DBG 0

// Uninitialized global data section
#define UDATA __attribute__((section(".ccudata")))

#define K 1024 // one kilobyte

#define DATA_BYTES (16 * K)       // data segment size
#define TEXT_BYTES (16 * K)       // code segment size
#define TS_TBL_BYTES (2 * K)      // type size table size (released at run time)
#define AST_TBL_BYTES (32 * K)    // abstract syntax table size (released at run time)
#define MEMBER_DICT_BYTES (4 * K) // struct member table size (released at run time)

#define CTLC 3 // control C ascii character

// Number of bits for parameter count
#define ADJ_BITS 5
#define ADJ_MASK ((1 << ADJ_BITS) - 1)

struct define_grp {
    const char* name; // function group name
    const int val;    // index of 1st function in group
};

union conv {                                           //
    int i;                                             // integer value
    float f;                                           // floating point value
};                                                     // current token value

// patch list entry
struct patch_s {
    struct patch_s* next; // list link
    struct patch_s* locs; // list of patch locations for this address
    uint16_t* addr;       // patched address
    int val;              // patch value
    int ext;              // is external function address
};

// relocation list entry
struct reloc_s {
    struct reloc_s* next; // list link
    int addr;             // address
};

extern struct reloc_s* relocs UDATA; // relocation list root
extern int nrelocs UDATA;            // relocation list size

extern char *p UDATA, *lp UDATA;                       // current position in source code
extern char* data UDATA;                               // data/bss pointer
extern char* data_base UDATA;                          // data/bss pointer
extern int* base_sp UDATA;                             // stack
extern uint16_t *e UDATA, *le UDATA, *text_base UDATA; // current position in emitted code
extern uint16_t* ecas UDATA;                           // case statement patch-up pointer
extern int* ncas UDATA;                                // case statement patch-up pointer
extern uint16_t* def UDATA;                            // default statement patch-up pointer
extern struct patch_s* brks UDATA;                     // break statement patch-up pointer
extern struct patch_s* cnts UDATA;                     // continue statement patch-up pointer
extern struct patch_s* pcrel UDATA;                    // pc relative address patch-up pointer
extern uint16_t* pcrel_1st UDATA;                      // first relative load address in group
extern int pcrel_count UDATA;                          // first relative load address in group
extern int swtc UDATA;                                 // !0 -> in a switch-stmt context
extern int brkc UDATA;                                 // !0 -> in a break-stmt context
extern int cntc UDATA;                                 // !0 -> in a continue-stmt context
extern int* tsize UDATA;                               // array (indexed by type) of type sizes
extern int tnew UDATA;                                 // next available type
extern int tk UDATA;                                   // current token
extern union conv tkv UDATA;                           // current token value
extern int ty UDATA;                                   // current expression type
                                                       // bit 0:1 - tensor rank, eg a[4][4][4]
                                                       // 0=scalar, 1=1d, 2=2d, 3=3d
                                                       //   1d etype -- bit 0:30)
                                                       //   2d etype -- bit 0:15,16:30 [32768,65536]
extern int compound UDATA;            // manage precedence of compound assign expressions
extern int rtf UDATA, rtt UDATA;      // return flag and return type for current function
extern int loc UDATA;                 // local variable offset
extern int lineno UDATA;              // current line number
extern int src_opt UDATA;             // print source and assembly flag
extern int nopeep_opt UDATA;          // turn off peep-hole optimization
extern int uchar_opt UDATA;           // use unsigned character variables
extern int* n UDATA;                  // current position in emitted abstract syntax tree
                                      // With an AST, the compiler is not limited to generate
                                      // code on the fly with parsing.
                                      // This capability allows function parameter code to be
                                      // emitted and pushed on the stack in the proper
                                      // right-to-left order.
extern int ld UDATA;                  // local variable depth
extern int pplev UDATA, pplevt UDATA; // preprocessor conditional level
extern int* ast UDATA;                // abstract syntax tree
extern ARMSTATE state UDATA;          // disassembler state
extern int exit_sp UDATA;                    // stack at entry to main
extern char* ofn UDATA;               // output file (executable) name
extern int indef UDATA;               // parsing in define statement
extern char* src_base UDATA;          // source code region

// identifier
struct ident_s {
    struct ident_s* next;
    int tk;     // type-id or keyword
    int hash;   // keyword hash
    char* name; // name of this identifier (not NULL terminated)
    /* fields starting with 'h' were designed to save and restore
     * the global class/type/val in order to handle the case if a
     * function declares a local with the same name as a global.
     */
    int class, hclass;    // FUNC, GLO (global var), LOC (local var), Syscall
    int type, htype;      // data type such as char and int
    int val, hval;        // address of symbol
    int etype, hetype;    // extended type info. different meaning for funcs.
    uint16_t* forward;    // forward call patch address
    uint8_t inserted : 1; // inserted in disassembler table
};

// symbol table
extern struct ident_s* id UDATA;       // currently parsed identifier
extern struct ident_s* sym_base UDATA; // symbol table (simple list of identifiers)

// struct member list entry
struct member_s {
    struct member_s* next; // list link
    struct ident_s* id;    // identifier name
    int offset;            // offset within struct
    int type;              // type
    int etype;             // extended type
};

extern struct member_s** members UDATA; // array (indexed by type) of struct member lists

// file control block
struct file_handle {
    struct file_handle* next; // list link
    bool is_dir;              // bool, is a directory file
    union {
        lfs_file_t file; // LFS file control block
        lfs_dir_t dir;   // LFS directory control block
    } u;
};

extern struct file_handle* file_list UDATA; // file list root

// types -- 4 scalar types, 1020 aggregate types, 4 tensor ranks, 8 ptr levels
// bits 0-1 = tensor rank, 2-11 = type id, 12-14 = ptr level
// 4 type ids are scalars: 0 = char/void, 1 = int, 2 = float, 3 = reserved
enum { CHAR = 0, INT = 4, FLOAT = 8, ATOM_TYPE = 11, PTR = 0x1000, PTR2 = 0x2000 };

// fatal erro message and exit
#define fatal(fmt, ...) fatal_func(__FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)

__attribute__((__noreturn__))
void fatal_func(const char* func, int lne, const char* fmt, ...);

__attribute__((__noreturn__))
void run_fatal(const char* fmt, ...);

#define COMPOUND 0x10000

void next();
void expr(int lev);

// help group definitions
struct help_grp {
    const char* name;
    const struct define_grp* grp;
};

extern const struct help_grp includes[];

// external function table entry
struct externs_s {
    const char* name;             // function name
    const int etype;              // function return and parameter type
    const struct define_grp* grp; // help group
    const void* extrn;            // function address
    const int ret_float : 1;      // returns float
    const int is_printf : 1;      // printf function
    const int is_sprintf : 1;     // sprintf function
};

size_t numof_externs();

extern const struct externs_s externs[];

int extern_search(char* name);
void typecheck(int op, int tl, int tr);
bool is_power_of_2(int n);
void bitopcheck(int tl, int tr);

void check_pc_relative(void);

extern void (*fops[])();

enum {
    aeabi_idiv = 1,
    aeabi_i2f,
    aeabi_f2iz,
    aeabi_fadd,
    aeabi_fsub,
    aeabi_fmul,
    aeabi_fdiv,
    aeabi_fcmple,
    aeabi_fcmpgt,
    aeabi_fcmplt,
    aeabi_fcmpge
};

#endif
