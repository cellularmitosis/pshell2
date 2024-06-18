#include "cc_gen.h"
#include "cc_internals.h"
#include "cc_ast.h"
#include "cc_tokns.h"
#include "cc_malloc.h"
#include "cc_ops.h"
#include "cc_peep.h"
#include "cc_wraps.h"

// ARM CM0+ code emitters

void emit(uint16_t n) {
    if (e >= text_base + (TEXT_BYTES / sizeof(*e)) - 1) {
        fatal("code segment exceeded, program is too big");
    }
    *++e = n;
    if (!nopeep_opt) {
        peep();
    }
}

static void emit_branch(uint16_t* to);
static void emit_cond_branch(uint16_t* to, int cond);

void emit_word(uint32_t n) {
    if (((int)e & 2) == 0) {
        fatal("mis-aligned word");
    }
    ++e;
    if (e >= text_base + (TEXT_BYTES / sizeof(*e)) - 2) {
        fatal("code segment exceeded, program is too big");
    }
    *((uint32_t*)e) = n;
    ++e;
}

static void emit_load_long_imm(int r, int val, int ext) {
    emit(0x4800 | (r << 8)); // ldr rr,[pc + offset n]
    struct patch_s* p = pcrel;
    while (p) {
        if (p->val == val) {
            break;
        }
        p = p->next;
    }
    if (!p) {
        ++pcrel_count;
        if (pcrel_1st == 0) {
            pcrel_1st = e;
        }
        p = cc_malloc(sizeof(struct patch_s), 1);
        p->val = val;
        p->ext = ext;
        if (pcrel == 0) {
            pcrel = p;
        } else {
            struct patch_s* p2 = pcrel;
            while (p2->next) {
                p2 = p2->next;
            }
            p2->next = p;
        }
    }
    struct patch_s* pl = cc_malloc(sizeof(struct patch_s), 1);
    pl->addr = e;
    pl->next = p->locs;
    p->locs = pl;
}

static void emit_load_immediate(int r, int val) {
    if (val >= 0 && val < 256) {       //
        emit(0x2000 | val | (r << 8)); // movs rr, #n
        return;
    }
    if (-val >= 0 && -val < 256) {
        emit(0x2000 | -val | (r << 8)); // movs rr, #n
        emit(0x4240 | (r << 3) | r);    // negs rr, rr
        return;
    }
    emit_load_long_imm(r, val, 0);
}

static void patch_pc_relative(int brnch) {
    int rel_count = pcrel_count;
    pcrel_count = 0;
    if (brnch) {
        if ((int)e & 2) {
            emit(0x46c0); // nop ; (mov r8, r8)
        }
        emit_branch(e + 2 * rel_count);
    } else {
        if (!((int)e & 2)) {
            emit(0x46c0); // nop ; (mov r8, r8)
        }
    }
    while (pcrel) {
        struct patch_s* p = pcrel;
        while (p->locs) {
            struct patch_s* pl = p->locs;
            if ((*pl->addr & 0x4800) != 0x4800) {
                fatal("unexpected compiler error");
            }
            int te = (int)e + 2;
            int ta = (int)pl->addr + 2;
            if (ta & 2) {
                ++ta;
            }
            int ofs = (te - ta) / 4;
            if (ofs > 255) {
                fatal("unexpected compiler error");
            }
            *pl->addr |= ofs;
            p->locs = pl->next;
            cc_free(pl);
        }
        emit_word(p->val);
        if (ofn && p->ext) {
            struct reloc_s* r = cc_malloc(sizeof(struct reloc_s), 1);
            r->addr = (int)(e - 1);
            r->next = relocs;
            relocs = r;
            nrelocs++;
        }
        pcrel = p->next;
        cc_free(p);
    }
    pcrel_1st = 0;
}

void check_pc_relative(void) {
    if (pcrel_1st == 0) {
        return;
    }
    int te = (int)e + 4 * pcrel_count;
    int ta = (int)pcrel_1st;
    if ((te - ta) > 1000) {
        patch_pc_relative(1);
    }
}

static void emit_enter(int n) {
    emit(0xb580);             // push {r7,lr}
    emit(0x466f);             // mov  r7, sp
    if (n) {                  //
        if (n < 128) {        //
            emit(0xb080 | n); // sub  sp, #n
        } else {              //
            emit_load_immediate(3, -n * 4);
            emit(0x449d); // add sp, r3
        }
    }
}

static void emit_leave(void) {
    emit(0x46bd); // mov sp, r7
    emit(0xbd80); // pop {r7, pc}
}

static void emit_load_addr(int n) {
    emit_load_immediate(0, (n) * 4);
    emit(0x4438); // add r0,r7
}

static void emit_push(int n) {
    emit(0xb400 | (1 << n)); // push {rn}
}

static void emit_pop(int n) {
    emit(0xbc00 | (1 << n)); // pop {rn}
}

static void emit_store(int n) {
    emit_pop(3);
    switch (n) {
    case SC:
        emit(0x7018); // strb r0,[r3,#0]
        break;
    case SI:
    case SF:
        emit(0x6018); // str r0,[r3,#0]
        break;
    default:
        fatal("unexpected compiler error");
    }
}

static void emit_load(int n) {
    switch (n) {
    case LC:
        emit(0x7800); // ldrb r0,[r0,#0]
        if (!uchar_opt) {
            emit(0xb240); // sxtb r0,r0
        }
        break;
    case LI:
    case LF:
        emit(0x6800); // ldr r0, [r0, #0]
        break;
    default:
        fatal("unexpected compiler error");
    }
}

static uint16_t* emit_call(int n);

static void emit_branch(uint16_t* to) {
    int ofs = to - (e + 1);
    if (ofs >= -1024 && ofs < 1024) {
        emit(0xe000 | (ofs & 0x7ff)); // JMP n
    } else {
        emit_call((int)(to + 2));
    }
}

static void emit_fop(int n) {
    if (!ofn) {
        emit_load_long_imm(3, (int)fops[n], 0);
    } else {
        emit_load_long_imm(3, -n, 1);
    }
    emit(0x4798); // blx r3
}

static void emit_cond_branch(uint16_t* to, int cond) {
    int ofs = to - (e + 1);
    if (ofs >= -128 && ofs < 128) {
        switch (cond) {
        case BZ:
            emit(0xd000 | (ofs & 0xff)); // be to
            break;
        case BNZ:
            emit(0xd100 | (ofs & 0xff)); // bne to
            break;
        default:
            fatal("unexpected compiler error");
        }
        return;
    }
    if (ofs >= -1023 && ofs < 1024) {
        switch (cond) {
        case BZ:
            emit(0xd100); // bne *+2
            break;
        case BNZ:
            emit(0xd000); // be *+2
            break;
        default:
            fatal("unexpected compiler error");
        }
        --ofs;
        emit(0xe000 | (ofs & 0x7ff)); // JMP to
        return;
    }
    switch (cond) {
    case BZ:
        emit(0xd101); // bne *+3
        break;
    case BNZ:
        emit(0xd001); // be *+3
        break;
    default:
        fatal("unexpected compiler error");
    }
    emit_call((int)to); // JMP to
}

static void emit_oper(int op) {
    switch (op) {
    case OR:
        emit_pop(3);
        emit(0x4318); // orrs r0,r3
        break;
    case XOR:
        emit_pop(3);
        emit(0x4058); // eors r0,r3
        break;
    case AND:
        emit_pop(3);
        emit(0x4018); // ands r0,r3
        break;
    case SHL:
        emit(0x4603); // mov r3,r0
        emit_pop(0);
        emit(0x4098); // lsls r0,r3
        break;
    case SHR:
        emit(0x4603); // mov r3, r0
        emit_pop(0);
        emit(0x4118); // asrs r0,r3
        break;
    case SUB:
        emit_pop(3);
        emit(0x1a18); // subs r0,r3,r0;
        break;
    case ADD:
        emit_pop(3);
        emit(0x18c0); // adds r0,r3;
        break;
    case MUL:
        emit_pop(3);
        emit(0x4358); // muls r0,r3;
        break;

    case EQ:
        emit_pop(1);
        emit(0x1a08); // subs r0,r1,r0
        emit(0x4243); // negs r3,r0 //FIX
        emit(0x4158); // adcs r0,r3
        break;
    case NE:
        emit_pop(1);
        emit(0x1a08); // subs r0,r1,r0
        emit(0x1e43); // subs r3,r0,#1 //FIX
        emit(0x4198); // sbcs r0,r3
        break;
    case GE:
        emit_pop(1);
        emit(0x0003); // movs r3,r0
        emit(0x17c8); // asrs r0,r1,#31
        emit(0x0fda); // lsrs r2,r3,#31
        emit(0x4299); // cmp  r1,r3
        emit(0x4150); // adcs r0,r2
        break;
    case LT:
        emit_pop(1);
        emit(0x2301); // movs r3,#1
        emit(0x4281); // cmp  r1,r0
        emit(0xdb00); // blt.n L2
        emit(0x2300); // movs r3,#0
                      // L2:
        emit(0x0018); // movs r0,r3
        break;
    case GT:
        emit_pop(1);
        emit(0x2301); // movs r3,#1
        emit(0x4281); // cmp  r1,r0
        emit(0xdc00); // bgt.n L1
        emit(0x2300); // movs r3,#0
                      // L1:
        emit(0x0018); // movs r0,r3
        break;
    case LE:
        emit_pop(1);
        emit(0x0003); // movs r3,r0
        emit(0x0fc8); // lsrs r0,r1,#31
        emit(0x17da); // asrs r2,r3,#31
        emit(0x428b); // cmp  r3,r1
        emit(0x4150); // adcs r0,r2
        break;

    case DIV:
    case MOD:
        emit(0x4601); // mov r1,r0
        emit_pop(0);  // pop {r0}
        emit_fop(aeabi_idiv);
        if (op == MOD) {
            emit(0x4608); // mov r0,r1
        }
        break;
    default:
        fatal("unexpected compiler error");
    }
}

static void emit_float_oper(int op) {
    switch (op) {
    case ADDF:
        emit_pop(1); // pop {r1}
        emit_fop((int)aeabi_fadd);
        break;
    case SUBF:
        emit(0x0001); // movs r1,r0
        emit_pop(0);
        emit_fop((int)aeabi_fsub);
        break;
    case MULF:
        emit_pop(1);
        emit_fop((int)aeabi_fmul);
        break;
    case DIVF:
        emit(0x0001); // movs r1,r0
        emit_pop(0);
        emit_fop((int)aeabi_fdiv);
        break;
    case GEF:
        emit(0x0001); // movs r1,r0
        emit_pop(0);
        emit_fop((int)aeabi_fcmpge);
        break;
    case GTF:
        emit_pop(1);
        emit_fop((int)aeabi_fcmple);
        break;
    case LTF:
        emit_pop(1);
        emit_fop((int)aeabi_fcmpge);
        break;
    case LEF:
        emit(0x0001); // movs r1,r0
        emit_pop(0);
        emit_fop((int)aeabi_fcmple);
        break;
    case EQF:
        emit_oper(EQ);
        break;
    case NEF:
        emit_oper(NE);
        break;

    default:
        fatal("unexpected compiler error");
    }
}

static void emit_cast(int n) {
    switch (n) {
    case ITOF:
        emit_fop((int)aeabi_i2f);
        break;
    case FTOI:
        emit_fop((int)aeabi_f2iz);
        break;
    default:
        fatal("unexpected compiler error");
    }
}

static void emit_adjust_stack(int n) {
    if (n) {
        emit(0xb000 | n); // add sp, #n*4
    }
}

static uint16_t* emit_call(int n) {
    if (n == 0) {
        emit(0);
        emit(0);
        return e - 1;
    }
    int ofs = (n - ((int)e + 6)) / 2;
    if (ofs < -8388608 || ofs > 8388607) {
        fatal("subroutine call too far");
    }
    int s = (ofs >> 31) & 1;
    int i1 = ((ofs >> 22) & 1) ^ 1;
    int i2 = ((ofs >> 21) & 1) ^ 1;
    int j1 = s ^ i1;
    int j2 = s ^ i2;
    int i11 = ofs & ((1 << 11) - 1);
    int i10 = (ofs >> 11) & ((1 << 10) - 1);
    emit(0xf000 | (s << 10) | i10);
    emit(0xd000 | (j1 << 13) | (j2 << 11) | i11);
    return e - 1;
}

static void emit_syscall(int n, int np) {
    const struct externs_s* p = externs + n;
    if (p->is_printf) {
        emit_load_immediate(0, np);
        if (!ofn) {
            emit_load_long_imm(3, (int)x_printf, 0);
        } else {
            emit_load_long_imm(3, n, 1);
        }
    } else if (p->is_sprintf) {
        emit_load_immediate(0, np);
        if (!ofn) {
            emit_load_long_imm(3, (int)x_sprintf, 0);
        } else {
            emit_load_long_imm(3, n, 1);
        }
    } else {
        int nparm = np & ADJ_MASK;
        if (nparm > 4) {
            nparm = 4;
        }
        while (nparm--) {
            emit_pop(nparm);
        }
        if (!ofn) {
            emit_load_long_imm(3, (int)p->extrn, 1);
        } else {
            emit_load_long_imm(3, n, 1);
        }
    }
    emit(0x4798); // blx r3
    int nparm = np & ADJ_MASK;
    if (p->is_printf || p->is_sprintf) {
        emit_adjust_stack(nparm);
    } else {
        nparm = (nparm > 4) ? nparm - 4 : 0;
        if (nparm) {
            emit_adjust_stack(nparm);
        }
    }
}

static void patch_branch(uint16_t* from, uint16_t* to) {
    if (*from != 0 || *(from + 1) != 0) {
        fatal("unexpected compiler error");
    }
    uint16_t* se = e;
    e = from - 1;
    emit_call((int)to);
    e = se;
}

// AST parsing for Thumb code generatiion

void gen(int* n) {
    int i = ast_Tk(n), j, k, l;
    uint16_t *a, *b, *c, *d, *t;
    struct ident_s* label;
    struct patch_s* patch;

    check_pc_relative();

    switch (i) {
    case Num:
    case NumF:
        emit_load_immediate(0, Num_entry(n).val);
        break; // int or float value
    case Load:
        gen(n + Load_words);                                          // load the value
        if (Num_entry(n).val > ATOM_TYPE && Num_entry(n).val < PTR) { // unreachable?
            fatal("struct copies not yet supported");
        }
        emit_load((Num_entry(n).val >= PTR) ? LI : LC + (Num_entry(n).val >> 2));
        break;
    case Loc:
        emit_load_addr(Num_entry(n).val);
        break; // get address of variable
    case '{':
        gen(Begin_entry(n).next);
        gen(n + Begin_words);
        break;   // parse AST expr or stmt
    case Assign: // assign the value to variables
        gen((int*)Assign_entry(n).right_part);
        emit_push(0);
        gen(n + Assign_words); // xxxx
        l = Num_entry(n).val & 0xffff;
        // Add SC/SI instruction to save value in register to variable address
        // held on stack.
        if (l > ATOM_TYPE && l < PTR) {
            fatal("struct assign not yet supported");
        }
        if ((Num_entry(n).val >> 16) == FLOAT && l == INT) {
            emit_fop((int)aeabi_f2iz);
        } else if ((Num_entry(n).val >> 16) == INT && l == FLOAT) {
            emit_fop((int)aeabi_i2f);
        }
        emit_store((l >= PTR) ? SI : SC + (l >> 2));
        break;
    case Inc: // increment or decrement variables
    case Dec:
        gen(n + Oper_words);
        emit_push(0);
        emit_load((Num_entry(n).val == CHAR) ? LC : LI);
        emit_push(0);
        emit_load_immediate(
            0, (Num_entry(n).val >= PTR2)
                   ? sizeof(int)
                   : ((Num_entry(n).val >= PTR) ? tsize[(Num_entry(n).val - PTR) >> 2] : 1));
        emit_oper((i == Inc) ? ADD : SUB);
        emit_store((Num_entry(n).val == CHAR) ? SC : SI);
        break;
    case Cond:                              // if else condition case
        gen((int*)Cond_entry(n).cond_part); // condition
        // Add jump-if-zero instruction "BZ" to jump to false branch.
        // Point "b" to the jump address field to be patched later.
        emit(0x2800); // cmp r0,#0
        emit_cond_branch(e + 2, BNZ);
        b = emit_call(0);
        gen((int*)Cond_entry(n).if_part); // expression
        // Patch the jump address field pointed to by "b" to hold the address
        // of false branch. "+ 3" counts the "JMP" instruction added below.
        //
        // Add "JMP" instruction after true branch to jump over false branch.
        // Point "b" to the jump address field to be patched later.
        if (Cond_entry(n).else_part) {
            patch_branch(b, e + 3);
            b = emit_call(0);
            gen((int*)Cond_entry(n).else_part);
        } // else statment
        // Patch the jump address field pointed to by "d" to hold the address
        // past the false branch.
        patch_branch(b, e + 1);
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
        gen((int*)Num_entry(n).val);
        emit(0x2800); // cmp r0,#0
        emit_cond_branch(e + 2, BZ);
        b = emit_call(0);
        gen(n + Oper_words);
        patch_branch(b, e + 1);
        break;
    case Lan:
        gen((int*)Num_entry(n).val);
        emit(0x2800); // cmp r0,#0
        emit_cond_branch(e + 2, BNZ);
        b = emit_call(0);
        gen(n + Oper_words);
        patch_branch(b, e + 1);
        break;
    /* If current token is bitwise OR operator:
     * Add "PSH" instruction to push LHS value in register to stack.
     * Parse RHS expression.
     * Add "OR" instruction to compute the result.
     */
    case Or:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(OR);
        break;
    case Xor:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(XOR);
        break;
    case And:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(AND);
        break;
    case Eq:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(EQ);
        break;
    case Ne:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(NE);
        break;
    case Ge:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(GE);
        break;
    case Lt:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(LT);
        break;
    case Gt:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(GT);
        break;
    case Le:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(LE);
        break;
    case Shl:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(SHL);
        break;
    case Shr:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(SHR);
        break;
    case Add:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(ADD);
        break;
    case Sub:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(SUB);
        break;
    case Mul:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(MUL);
        break;
    case Div:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(DIV);
        break;
    case Mod:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_oper(MOD);
        break;
    case AddF:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_float_oper(ADDF);
        break;
    case SubF:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_float_oper(SUBF);
        break;
    case MulF:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_float_oper(MULF);
        break;
    case DivF:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_float_oper(DIVF);
        break;
    case EqF:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_float_oper(EQF);
        break;
    case NeF:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_float_oper(NEF);
        break;
    case GeF:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_float_oper(GEF);
        break;
    case LtF:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_float_oper(LTF);
        break;
    case GtF:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_float_oper(GTF);
        break;
    case LeF:
        gen((int*)Num_entry(n).val);
        emit_push(0);
        gen(n + Oper_words);
        emit_float_oper(LEF);
        break;
    case CastF:
        gen((int*)CastF_entry(n).val);
        emit_cast(CastF_entry(n).way);
        break;
    case Func:
    case Syscall:
        b = (uint16_t*)Func_entry(n).next;
        k = b ? Func_entry(n).n_parms : 0;
        int sj = 0;
        if (k) {
            l = Func_entry(n).parm_types >> 10;
            int* t;
            t = cc_malloc(sizeof(int) * (k + 1), 1);
            j = 0;
            while (ast_Tk(b)) {
                t[j++] = (int)b;
                b = (uint16_t*)ast_Tk(b);
            }
            int sj = j;
            while (j >= 0) { // push arguments
                gen((int*)b + 1);
                emit_push(0);
                --j;
                b = (uint16_t*)t[j];
            }
            cc_free(t);
        }
        if (i == Syscall) {
            emit_syscall(Func_entry(n).addr, Func_entry(n).parm_types);
        } else if (i == Func) {
            emit_call(Func_entry(n).addr);
            emit_adjust_stack(Func_entry(n).n_parms);
        }
        break;
    case While:
    case DoWhile:
        if (i == While) {
            a = emit_call(0);
        }
        b = (uint16_t*)brks;
        brks = 0;
        c = (uint16_t*)cnts;
        cnts = 0;
        d = e;
        gen((int*)While_entry(n).body); // loop body
        if (i == While) {
            patch_branch(a, e + 1);
        }
        while (cnts) {
            t = (uint16_t*)cnts->next;
            patch_branch(cnts->addr, e + 1);
            cc_free(cnts);
            cnts = (struct patch_s*)t;
        }
        cnts = (struct patch_s*)c;
        gen((int*)While_entry(n).cond); // condition
        emit(0x2800);                   // cmp r0,#0
        emit_cond_branch(d - 1, BNZ);
        while (brks) {
            t = (uint16_t*)brks->next;
            patch_branch(brks->addr, e + 1);
            cc_free(brks);
            brks = (struct patch_s*)t;
        }
        brks = (struct patch_s*)b;
        break;
    case For:
        gen((int*)For_entry(n).init); // init
        a = emit_call(0);
        b = (uint16_t*)brks;
        brks = 0;
        c = (uint16_t*)cnts;
        cnts = 0;
        gen((int*)For_entry(n).body); // loop body
        uint16_t* t2;
        while (cnts) {
            t = (uint16_t*)cnts->next;
            t2 = e;
            patch_branch(cnts->addr, e + 1);
            cc_free(cnts);
            cnts = (struct patch_s*)t;
        }
        cnts = (struct patch_s*)c;
        gen((int*)For_entry(n).incr); // increment
        patch_branch(a, e + 1);
        if (For_entry(n).cond) {
            gen((int*)For_entry(n).cond); // condition
            emit(0x2800);                 // cmp r0,#0
            emit_cond_branch(a, BNZ);
        } else {
            emit_branch(a);
        }
        while (brks) {
            t = (uint16_t*)brks->next;
            patch_branch(brks->addr, e + 1);
            cc_free(brks);
            brks = (struct patch_s*)t;
        }
        brks = (struct patch_s*)b;
        break;
    case Switch:
        gen((int*)Switch_entry(n).cond); // condition
        emit_push(0);
        a = ecas;
        ecas = emit_call(0);
        b = (uint16_t*)brks;
        d = def;
        def = 0;
        brks = 0;
        gen((int*)Switch_entry(n).cas); // case statment
        // deal with no default inside switch case
        patch_branch(ecas, (def ? def : e) + 1);
        while (brks) {
            t = (uint16_t*)brks->next;
            patch_branch((uint16_t*)(brks->addr), e + 1);
            cc_free(brks);
            brks = (struct patch_s*)t;
        }
        emit_adjust_stack(1);
        brks = (struct patch_s*)b;
        def = d;
        break;
    case Case:
        a = 0;
        patch_branch(ecas, e + 1);
        gen((int*)Num_entry(n).val); // condition
        // if (*(e - 1) != IMM) // ***FIX***
        //    fatal("case label not a numeric literal");
        emit(0x9b00); // ldr r3, [sp, #0]
        emit(0x4298); // cmp r0, r3
        emit_cond_branch(e + 2, BZ);
        ecas = emit_call(0);
        if (*((int*)Case_entry(n).expr) == Switch) {
            a = ecas;
        }
        gen((int*)Case_entry(n).expr); // expression
        if (a != 0) {
            ecas = a;
        }
        break;
    case Break:
        patch = cc_malloc(sizeof(struct patch_s), 1);
        patch->addr = emit_call(0);
        patch->next = brks;
        brks = patch;
        break;
    case Continue:
        patch = cc_malloc(sizeof(struct patch_s), 1);
        patch->next = cnts;
        patch->addr = emit_call(0);
        cnts = patch;
        break;
    case Goto:
        label = (struct ident_s*)Num_entry(n).val;
        if (label->class == 0) {
            struct patch_s* l = cc_malloc(sizeof(struct patch_s), 1);
            l->addr = emit_call(0);
            l->next = (struct patch_s*)label->forward;
            label->forward = (uint16_t*)l;
        } else {
            emit_branch((uint16_t*)label->val - 1);
        }
        break;
    case Default:
        def = e;
        gen((int*)Num_entry(n).val);
        break;
    case Return:
        if (Num_entry(n).val) {
            gen((int*)Num_entry(n).val);
        }
        emit_leave();
        break;
    case Enter:
        emit_enter(Num_entry(n).val);
        gen(n + Enter_words);
        emit_leave();
        patch_pc_relative(0);
        break;
    case Label: // target of goto
        label = (struct ident_s*)Num_entry(n).val;
        if (label->class != 0) {
            fatal("duplicate label definition");
        }
        d = e;
        while (label->forward) {
            struct patch_s* l = (struct patch_s*)label->forward;
            patch_branch(l->addr, d + 1);
            label->forward = (uint16_t*)l->next;
            cc_free(l);
        }
        label->val = (int)d;
        label->class = Label;
        break;
    default:
        if (i != ';') {
            fatal("%d: compiler error gen=%08x\n", lineno, i);
        }
    }
}
