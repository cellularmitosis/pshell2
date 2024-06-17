#ifndef _CC_AST_H_
#define _CC_AST_H_

#include <stdint.h>

// Abstract syntax tree entry creation

void push_ast(int l);

// Double

typedef struct {
    int tk;
    int v1;
} Double_entry_t;

#define Double_entry(a) (*((Double_entry_t*)a))
#define Double_words (sizeof(Double_entry_t) / sizeof(int))

// Func

typedef struct {
    int tk;
    int next;
    int addr;
    int n_parms;
    int parm_types;
} Func_entry_t;

#define Func_entry(a) (*((Func_entry_t*)a))
#define Func_words (sizeof(Func_entry_t) / sizeof(int))
void ast_Func(int parm_types, int n_parms, int addr, int next, int tk);

// For

typedef struct {
    int tk;
    int cond;
    int incr;
    int body;
    int init;
} For_entry_t;

#define For_entry(a) (*((For_entry_t*)a))
#define For_words (sizeof(For_entry_t) / sizeof(int))
void ast_For(int init, int body, int incr, int cond);

// Cond

typedef struct {
    int tk;
    int cond_part;
    int if_part;
    int else_part;
} Cond_entry_t;

#define Cond_entry(a) (*((Cond_entry_t*)a))
#define Cond_words (sizeof(Cond_entry_t) / sizeof(int))
void ast_Cond(int else_part, int if_part, int cond_part);

// Assign

typedef struct {
    int tk;
    int type;
    int right_part;
} Assign_entry_t;

#define Assign_entry(a) (*((Assign_entry_t*)a))
#define Assign_words (sizeof(Assign_entry_t) / sizeof(int))
void ast_Assign(int right_part, int type);

// While

typedef struct {
    int tk;
    int body;
    int cond;
} While_entry_t;

#define While_entry(a) (*((While_entry_t*)a))
#define While_words (sizeof(While_entry_t) / sizeof(int))
void ast_While(int cond, int body, int tk);

// Switch

typedef struct {
    int tk;
    int cond;
    int cas;
} Switch_entry_t;

#define Switch_entry(a) (*((Switch_entry_t*)a))
#define Switch_words (sizeof(Switch_entry_t) / sizeof(int))
void ast_Switch(int cas, int cond);

// Case

typedef struct {
    int tk;
    int next;
    int expr;
} Case_entry_t;

#define Case_entry(a) (*((Case_entry_t*)a))
#define Case_words (sizeof(Case_entry_t) / sizeof(int))
void ast_Case(int expr, int next);

// CastF

typedef struct {
    int tk;
    int val;
    int way;
} CastF_entry_t;

#define CastF_entry(a) (*((CastF_entry_t*)a))
#define CastF_words (sizeof(CastF_entry_t) / sizeof(int))
void ast_CastF(int way, int val);

// Enter

typedef struct {
    int tk;
    int val;
} Enter_entry_t;

#define Enter_entry(a) (*((Enter_entry_t*)a))
#define Enter_words (sizeof(Enter_entry_t) / sizeof(int))
uint16_t* ast_Enter(int val);

// Two word entries:

// Return

void ast_Return(int v1);

// Oper

typedef struct {
    int tk;
    int oprnd;
} Oper_entry_t;

#define Oper_entry(a) (*((Oper_entry_t*)a))
#define Oper_words (sizeof(Oper_entry_t) / sizeof(int))
void ast_Oper(int oprnd, int op);

// Num

typedef struct {
    int tk;
    int val;
    int valH;
} Num_entry_t;

#define Num_entry(a) (*((Num_entry_t*)a))
#define Num_words (sizeof(Num_entry_t) / sizeof(int))
void ast_Num(int val);

void ast_Label(int v1);
void ast_Goto(int v1);
void ast_Default(int v1);
void ast_NumF(int v1);
void ast_Loc(int addr);

// Load

typedef struct {
    int tk;
    int typ;
} Load_entry_t;

#define Load_entry(a) (*((Load_entry_t*)a))
#define Load_words (sizeof(Load_entry_t) / sizeof(int))
void ast_Load(int typ);

// Begin

typedef struct {
    int tk;
    int* next;
} Begin_entry_t;

#define Begin_entry(a) (*((Begin_entry_t*)a))
#define Begin_words (sizeof(Begin_entry_t) / sizeof(int))
void ast_Begin(int* next);

// Single word entry:

// Tk, Single

typedef struct {
    int tk;
} Single_entry_t;

#define Single_entry(a) (*((Single_entry_t*)a))
#define Single_words (sizeof(Single_entry_t) / sizeof(int))
#define ast_Tk(a) (Single_entry(a).tk)
void ast_Single(int k);

// End

typedef struct {
    int tk;
} End_entry_t;

#define End_entry(a) (*((End_entry_t*)a))
#define End_words (sizeof(End_entry_t) / sizeof(int))
void ast_End(void);

#endif
