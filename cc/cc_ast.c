#include "cc_ast.h"
#include "cc_internals.h"
#include "cc_tokns.h"

// Abstract syntax tree entry creation

void push_ast(int l) {
    n -= l;
    if (n < ast) {
        fatal("AST overflow compiler error. Program too big");
    }
}

void ast_Func(int parm_types, int n_parms, int addr, int next, int tk) {
    push_ast(Func_words);
    Func_entry(n).parm_types = parm_types;
    Func_entry(n).n_parms = n_parms;
    Func_entry(n).addr = addr;
    Func_entry(n).next = next;
    Func_entry(n).tk = tk;
}

void ast_For(int init, int body, int incr, int cond) {
    push_ast(For_words);
    For_entry(n).init = init;
    For_entry(n).body = body;
    For_entry(n).incr = incr;
    For_entry(n).cond = cond;
    For_entry(n).tk = For;
}

void ast_Cond(int else_part, int if_part, int cond_part) {
    push_ast(Cond_words);
    Cond_entry(n).else_part = else_part;
    Cond_entry(n).if_part = if_part;
    Cond_entry(n).cond_part = cond_part;
    Cond_entry(n).tk = Cond;
}

void ast_Assign(int right_part, int type) {
    push_ast(Assign_words);
    Assign_entry(n).right_part = right_part;
    Assign_entry(n).type = type;
    Assign_entry(n).tk = Assign;
}

void ast_While(int cond, int body, int tk) {
    push_ast(While_words);
    While_entry(n).cond = cond;
    While_entry(n).body = body;
    While_entry(n).tk = tk;
}

void ast_Switch(int cas, int cond) {
    push_ast(Switch_words);
    Switch_entry(n).cas = cas;
    Switch_entry(n).cond = cond;
    Switch_entry(n).tk = Switch;
}

void ast_Case(int expr, int next) {
    push_ast(Case_words);
    Case_entry(n).expr = expr;
    Case_entry(n).next = next;
    Case_entry(n).tk = Case;
}

void ast_CastF(int way, int val) {
    push_ast(CastF_words);
    CastF_entry(n).tk = CastF;
    CastF_entry(n).val = val;
    CastF_entry(n).way = way;
}

uint16_t* ast_Enter(int val) {
    push_ast(Enter_words);
    Enter_entry(n).tk = Enter;
    Enter_entry(n).val = val;
}

// Two word entries

void ast_Return(int v1) {
    push_ast(Double_words);
    Double_entry(n).tk = Return;
    Double_entry(n).v1 = v1;
}

void ast_Oper(int oprnd, int op) {
    push_ast(Oper_words);
    Oper_entry(n).tk = op;
    Oper_entry(n).oprnd = oprnd;
}

void ast_Num(int val) {
    push_ast(Num_words);
    Num_entry(n).tk = Num;
    Num_entry(n).val = val;
    Num_entry(n).valH = 0;
}

void ast_Label(int v1) {
    push_ast(Double_words);
    Double_entry(n).tk = Label;
    Double_entry(n).v1 = v1;
}

void ast_Goto(int v1) {
    push_ast(Double_words);
    Double_entry(n).tk = Goto;
    Double_entry(n).v1 = v1;
}

void ast_Default(int v1) {
    push_ast(Double_words);
    Double_entry(n).tk = Default;
    Double_entry(n).v1 = v1;
}

void ast_NumF(int v1) {
    push_ast(Num_words);
    Num_entry(n).tk = NumF;
    Num_entry(n).val = v1;
    Num_entry(n).valH = 0;
}

void ast_Loc(int addr) {
    push_ast(Double_words);
    Double_entry(n).tk = Loc;
    Double_entry(n).v1 = addr;
}

void ast_Load(int typ) {
    push_ast(Load_words);
    Load_entry(n).tk = Load;
    Load_entry(n).typ = typ;
}

void ast_Begin(int* next) {
    push_ast(Begin_words);
    Begin_entry(n).tk = '{';
    Begin_entry(n).next = next;
}

// Single word entry

void ast_Single(int k) {
    push_ast(Single_words);
    Single_entry(n).tk = k;
}

void ast_End(void) {
    push_ast(End_words);
    End_entry(n).tk = ';';
}
