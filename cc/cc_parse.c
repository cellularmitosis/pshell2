#include "cc_parse.h"
#include "cc_internals.h"
#include "cc_ast.h"
#include "cc_tokns.h"
#include "cc_malloc.h"
#include "cc_ops.h"
#include "cc_gen.h"
#include <stdio.h>

/* parse next token
 * 1. store data into id and then set the id to current lexcial form
 * 2. set tk to appropriate type
 */
void next() {
    char *pp, *tp, tc;
    int t, t2;
    struct ident_s* i2;

    /* using loop to ignore whitespace characters, but characters that
     * cannot be recognized by the lexical analyzer are considered blank
     * characters, such as '@' and '$'.
     */
    while ((tk = *p)) {
        ++p;
        if ((tk >= 'a' && tk <= 'z') || (tk >= 'A' && tk <= 'Z') || (tk == '_')) {
            pp = p - 1;
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || (*p == '_')) {
                tk = tk * 147 + *p++;
            }
            tk = (tk << 6) + (p - pp); // hash plus symbol length
            // hash value is used for fast comparison. Since it is inaccurate,
            // we have to validate the memory content as well.
            id = sym_base;
            for (id = sym_base; id; id = id->next) { // find one free slot in table
                if (tk == id->hash &&                // if token is found (hash match), overwrite
                    !memcmp(id->name, pp, p - pp)) {
                    tk = id->tk;
                    return;
                }
            }
            /* At this point, existing symbol name is not found.
             * "id" points to the first unused symbol table entry.
             */
            id = cc_malloc(sizeof(struct ident_s), 1);
            id->name = pp;
            id->hash = tk;
            id->forward = 0;
            id->inserted = 0;
            tk = id->tk = Id; // token type identifier
            id->next = sym_base;
            sym_base = id;
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
                printf("%d: %.*s", lineno, p - lp, lp);
                lp = p;
            }
            ++lineno;
            if (indef) {
                indef = 0;
                tk = ';';
                return;
            }
        case ' ':
        case '\t':
        case '\v':
        case '\f':
        case '\r':
            break;
        case '/':
            if (*p == '/') { // comment
                while (*p != 0 && *p != '\n') {
                    ++p;
                }
            } else if (*p == '*') { // C-style multiline comments
                for (++p; (*p != 0); ++p) {
                    pp = p + 1;
                    if (*p == '\n') {
                        ++lineno;
                    } else if (*p == '*' && *pp == '/') {
                        p += 1;
                        break;
                    }
                }
                if (*p) {
                    ++p;
                }
            } else {
                if (*p == '=') {
                    ++p;
                    tk = DivAssign;
                } else {
                    tk = Div;
                }
                return;
            }
            break;
        case '#': // skip include statements, and most preprocessor directives
            if (!strncmp(p, "define", 6)) {
                p += 6;
                next();
                i2 = id;
                // anything before eol?
                tp = p;
                while ((*tp == ' ') || (*tp == '\t')) {
                    ++tp;
                }
                if ((*tp != 0) && (*tp != '\n') && memcmp(tp, "//", 2) && memcmp(tp, "/*", 2)) {
                    // id->class = Glo;
                    indef = 1; // prevent recursive loop
                    next();
                    expr(Assign);
                    if ((ast_Tk(n) == Num) || (ast_Tk(n) == NumF)) {
                        id = i2;
                        id->class = ast_Tk(n);
                        id->type = ast_Tk(n) == Num ? INT : FLOAT;
                        id->val = Num_entry(n).val;
                        n += Num_words;
                        break;
                    } else {
                        fatal("define value must be a constant integer or float expression");
                    }
                } else {
                    id->class = Num;
                    id->type = INT;
                    id->val = 0;
                }
            } else if ((t = !strncmp(p, "ifdef", 5)) || !strncmp(p, "ifndef", 6)) {
                p += 6;
                next();
                if (tk != Id) {
                    fatal("No identifier");
                }
                ++pplev;
                if ((((id->class != Num && id->class != NumF) ? 0 : 1) ^ (t ? 1 : 0)) & 1) {
                    t = pplevt;
                    pplevt = pplev - 1;
                    while (*p != 0 && *p != '\n') {
                        ++p; // discard until end-of-line
                    }
                    do {
                        next();
                    } while (pplev != pplevt);
                    pplevt = t;
                }
            } else if (!strncmp(p, "if", 2)) {
                // ignore side effects of preprocessor if-statements
                ++pplev;
            } else if (!strncmp(p, "endif", 5)) {
                if (--pplev < 0) {
                    fatal("preprocessor context nesting error");
                }
                if (pplev == pplevt) {
                    return;
                }
            } else if (!strncmp(p, "pragma", 6)) {
                p += 6;
                while ((*p == ' ') || (*p == '\t')) {
                    ++p;
                }
                if (!strncmp(p, "uchar", 5)) {
                    uchar_opt = 1;
                }
            }
            while (*p != 0 && *p != '\n') {
                ++p; // discard until end-of-line
            }
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
                    case 'b':
                        tkv.i = '\b';
                        break; // backspace
                    case '0':
                    case '1':
                    case '2':
                    case '3':
                    case '4':
                    case '5':
                    case '6':
                    case '7':
                        t = tkv.i - '0';
                        t2 = 1;
                        while (*p >= '0' & *p <= '7') {
                            if (++t2 > 3) {
                                break;
                            }
                            t = (t << 3) + *p++ - '0';
                        }
                        if (t > 255) {
                            fatal("bad octal character in string");
                        }
                        tkv.i = t; // octal representation
                        break;
                    case 'x':
                    case 'X':
                        t = 0;
                        while ((*p >= '0' & *p <= '9') || (*p >= 'a' & *p <= 'f') ||
                               (*p >= 'A' & *p <= 'F')) {
                            if (*p >= '0' & *p <= '9') {
                                t = (t << 4) + *p++ - '0';
                            } else if (*p >= 'A' & *p <= 'F') {
                                t = (t << 4) + *p++ - 'A' + 10;
                            } else {
                                t = (t << 4) + *p++ - 'a' + 10;
                            }
                        }
                        if (t > 255) {
                            fatal("bad hexadecimal character in string");
                        }
                        tkv.i = t; // hexadecimal representation
                        break;     // an int with value 0
                    }
                }
                // if it is double quotes (string literal), it is considered as
                // a string, copying characters to data
                if (tk == '"') {
                    if (data >= data_base + DATA_BYTES) {
                        fatal("program data exceeds data segment");
                    }
                    *data++ = tkv.i;
                }
            }
            ++p;
            if (tk == '"') {
                tkv.i = (int)pp;
            } else {
                tk = Num;
            }
            return;
        case '=':
            if (*p == '=') {
                ++p;
                tk = Eq;
            } else {
                tk = Assign;
            }
            return;
        case '*':
            if (*p == '=') {
                ++p;
                tk = MulAssign;
            } else {
                tk = Mul;
            }
            return;
        case '+':
            if (*p == '+') {
                ++p;
                tk = Inc;
            } else if (*p == '=') {
                ++p;
                tk = AddAssign;
            } else {
                tk = Add;
            }
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
            } else {
                tk = Sub;
            }
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
            } else {
                tk = And;
            }
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
                } else {
                    tk = Shl;
                }
            } else {
                tk = Lt;
            }
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
                } else {
                    tk = Shr;
                }
            } else {
                tk = Gt;
            }
            return;
        case '|':
            if (*p == '|') {
                ++p;
                tk = Lor;
            } else if (*p == '=') {
                ++p;
                tk = OrAssign;
            } else {
                tk = Or;
            }
            return;
        case '^':
            if (*p == '=') {
                ++p;
                tk = XorAssign;
            } else {
                tk = Xor;
            }
            return;
        case '%':
            if (*p == '=') {
                ++p;
                tk = ModAssign;
            } else {
                tk = Mod;
            }
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

#define numof(a) (sizeof(a) / sizeof(a[0]))

// get cache index of external function
int extern_search(char* name)
{
    int first = 0, last = numof_externs() - 1, middle;
    while (first <= last) {
        middle = (first + last) / 2;
        if (strcmp(name, externs[middle].name) > 0) {
            first = middle + 1;
        } else if (strcmp(name, externs[middle].name) < 0) {
            last = middle - 1;
        } else {
            return middle;
        }
    }
    return -1;
}

// verify binary operations are legal
void typecheck(int op, int tl, int tr) {
    int pt = 0, it = 0, st = 0;
    if (tl >= PTR) {
        pt += 2; // is pointer?
    }
    if (tr >= PTR) {
        pt += 1;
    }

    if (tl < FLOAT) {
        it += 2; // is int?
    }
    if (tr < FLOAT) {
        it += 1;
    }

    if (tl > ATOM_TYPE && tl < PTR) {
        st += 2; // is struct/union?
    }
    if (tr > ATOM_TYPE && tr < PTR) {
        st += 1;
    }

    if ((tl ^ tr) & (PTR | PTR2)) { // operation on different pointer levels
        if (op == Add && pt != 3 && (it & ~pt))
            ; // ptr + int or int + ptr ok
        else if (op == Sub && pt == 2 && it == 1)
            ; // ptr - int ok
        else if (op == Assign && pt == 2 && ast_Tk(n) == Num && Num_entry(n).val == 0)
            ; // ok
        else if (op >= Eq && op <= Le && ast_Tk(n) == Num && Num_entry(n).val == 0)
            ; // ok
        else {
            fatal("bad pointer arithmetic or cast needed");
        }
    } else if (pt == 3 && op != Assign && op != Sub &&
               (op < Eq || op > Le)) { // pointers to same type
        fatal("bad pointer arithmetic");
    }

    if (pt == 0 && op != Assign && (it == 1 || it == 2)) {
        fatal("cast operation needed");
    }

    if (pt == 0 && st != 0) {
        fatal("illegal operation with dereferenced struct");
    }
}

void bitopcheck(int tl, int tr) {
    if (tl >= FLOAT || tr >= FLOAT) {
        fatal("bit operation on non-int types");
    }
}

bool is_power_of_2(int n) {
    return ((n - 1) & n) == 0;
}

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

void expr(int lev) {
    int t, tc, tt, nf, *b, sz, *c;
    int memsub = 0;
    struct ident_s* d;
    struct member_s* m;

    check_pc_relative();

    switch (tk) {
    case Id:
        d = id;
        next();
        // function call
        if (tk == '(') {
            if (d->class == Func && d->val == 0) {
                goto resolve_fnproto;
            }
            if (d->class < Func || d->class > Syscall) {
                if (d->class != 0) {
                    fatal("bad function call");
                }
                d->type = INT;
                d->etype = 0;
            resolve_fnproto:
                d->class = Syscall;
                int namelen = d->hash & 0x3f;
                char ch = d->name[namelen];
                d->name[namelen] = 0;
                int ix = extern_search(d->name);
                d->name[namelen] = ch;
                if (ix < 0) {
                    char* cp = cc_malloc(namelen + 1, 1);
                    memcpy(cp, d->name, namelen);
                    cp[namelen] = 0;
                    fatal("Unknown external function %s", cp);
                }
                d->val = ix;
                d->type = externs[ix].ret_float ? FLOAT : INT;
                d->etype = externs[ix].etype;
            }
            if (src_opt && !d->inserted) {
                d->inserted;
                int namelen = d->hash & 0x3f;
                char ch = d->name[namelen];
                d->name[namelen] = 0;
                if (d->class == Func) {
                    disasm_symbol(&state, d->name, d->val, ARMMODE_THUMB);
                } else {
                    disasm_symbol(&state, d->name, (int)externs[d->val].extrn | 1, ARMMODE_THUMB);
                }
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
                    ast_Begin(c);
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
                    if (tk == ')') {
                        fatal("unexpected comma in function call");
                    }
                } else if (tk != ')') {
                    fatal("missing comma in function call");
                }
            }
            if (t > ADJ_MASK) {
                fatal("maximum of %d function parameters", ADJ_MASK);
            }
            tt = (tt << 10) + (nf << 5) + t; // func etype not like other etype
            if (d->etype != tt) {
                if (d->class == Func) {
                    fatal("argument type mismatch");
                } else if (!externs[d->val].is_printf && !externs[d->val].is_sprintf) {
                    fatal("argument type mismatch");
                }
            }
            next();
            // function or system call id
            ast_Func(tt, t, d->val, (int)b, d->class);
            ty = d->type;
        }
        // enumeration, only enums have ->class == Num
        else if (d->class == Num) {
            ast_Num(d->val);
            ty = INT;
        } else if (d->class == NumF) {
            ast_Num(d->val);
            ty = FLOAT;
        } else if (d->class == Func) {
            ast_Num(d->val | 1);
            ty = INT;
        } else {
            // Variable get offset
            switch (d->class) {
            case Loc:
            case Par:
                ast_Loc(loc - d->val);
                break;
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
            if (data >= data_base + DATA_BYTES) {
                fatal("program data exceeds data segment");
            }
            next();
        }
        if (data >= data_base + DATA_BYTES) {
            fatal("program data exceeds data segment");
        }
        data = (char*)(((int)data + sizeof(int)) & (-sizeof(int)));
        ty = CHAR + PTR;
        break;
    /* SIZEOF_expr -> 'sizeof' '(' 'TYPE' ')'
     * FIXME: not support "sizeof (Id)".
     */
    case Sizeof:
        next();
        if (tk != '(') {
            fatal("open parenthesis expected in sizeof");
        }
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
                if (tk != Id || id->type <= ATOM_TYPE || id->type >= PTR) {
                    fatal("bad struct/union type");
                }
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
        if (tk != ')') {
            fatal("close parenthesis expected in sizeof");
        }
        next();
        ast_Num((ty & 3) ? (((ty - PTR) >= PTR) ? sizeof(int) : tsize[(ty - PTR) >> 2])
                         : ((ty >= PTR) ? sizeof(int) : tsize[ty >> 2]));
        // just one dimension supported at the moment
        //   1d etype -- bit 0:30)
        //   2d etype -- bit 0:15,16:30 [32768,65536]
        //   3d etype -- bit 0:10,11:20,21:30 [1024,1024,2048]
        // bit 2:9 - type
        // bit 10:11 - ptr level
        if ((d != 0) && (ty & 3)) {
            switch (ty & 3) {
            case 1:
                Num_entry(n).val *= (id->etype & 0x7fffffff) + 1;
                break;
            case 2:
                Num_entry(n).val *= ((id->etype & 0xffff) + 1) * (((id->etype >> 16) & 0x7fff) + 1);
                break;
            case 3:
                Num_entry(n).val *= ((id->etype & 0x3ff) + 1) * (((id->etype >> 11) & 0x3ff) + 1) *
                                    (((id->etype >> 21) & 0x7ff) + 1);
                break;
            }
        }
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
                if (tk != Id || id->type <= ATOM_TYPE || id->type >= PTR) {
                    fatal("bad struct/union type");
                }
                t = id->type;
                next();
                break;
            }
            // t: pointer
            while (tk == Mul) {
                next();
                t += PTR;
            }
            if (tk != ')') {
                fatal("bad cast");
            }
            next();
            expr(Inc); // cast has precedence as Inc(++)
            if (t != ty && (t == FLOAT || ty == FLOAT)) {
                if (t == FLOAT && ty < FLOAT) { // float : int
                    if (ast_Tk(n) == Num) {
                        ast_Tk(n) = NumF;
                        *((float*)&Num_entry(n).val) = Num_entry(n).val;
                    } else {
                        b = n;
                        ast_CastF(ITOF, (int)b);
                    }
                } else if (t < FLOAT && ty == FLOAT) { // int : float
                    if (ast_Tk(n) == NumF) {
                        ast_Tk(n) = Num;
                        Num_entry(n).val = *((float*)&Num_entry(n).val);
                    } else {
                        b = n;
                        ast_CastF(FTOI, (int)b);
                    }
                } else {
                    fatal("explicit cast required");
                }
            }
            ty = t;
        } else {
            expr(Assign);
            while (tk == ',') {
                next();
                b = n;
                expr(Assign);
                if (b != n) {
                    ast_Begin(b);
                }
            }
            if (tk != ')') {
                fatal("close parenthesis expected");
            }
            next();
        }
        break;
    case Mul: // "*", dereferencing the pointer operation
        next();
        expr(Inc); // dereference has the same precedence as Inc(++)
        if (ty < PTR) {
            fatal("bad dereference");
        }
        ty -= PTR;
        ast_Load(ty);
        break;
    case And: // "&", take the address operation
        /* when "token" is a variable, it takes the address first and
         * then LI/LC, so `--e` becomes the address of "a".
         */
        next();
        expr(Inc);
        if (ast_Tk(n) != Load) {
            fatal("bad address-of");
        }
        n += Load_words;
        ty += PTR;
        break;
    case '!': // "!x" is equivalent to "x == 0"
        next();
        expr(Inc);
        if (ty > ATOM_TYPE && ty < PTR) {
            fatal("!(struct/union) is meaningless");
        }
        if (ast_Tk(n) == Num) {
            Num_entry(n).val = !Num_entry(n).val;
        } else {
            ast_Num(0);
            ast_Oper((int)(n + Num_words), Eq);
        }
        ty = INT;
        break;
    case '~': // "~x" is equivalent to "x ^ -1"
        next();
        expr(Inc);
        if (ty > ATOM_TYPE) {
            fatal("~ptr is illegal");
        }
        if (ast_Tk(n) == Num) {
            Num_entry(n).val = ~Num_entry(n).val;
        } else {
            ast_Num(-1);
            ast_Oper((int)(n + Num_words), Xor);
        }
        ty = INT;
        break;
    case Add:
        next();
        expr(Inc);
        if (ty > ATOM_TYPE) {
            fatal("unary '+' illegal on ptr");
        }
        break;
    case Sub:
        next();
        expr(Inc);
        if (ty > ATOM_TYPE) {
            fatal("unary '-' illegal on ptr");
        }
        if (ast_Tk(n) == Num) {
            Num_entry(n).val = -Num_entry(n).val;
        } else if (ast_Tk(n) == NumF) {
            Num_entry(n).val ^= 0x80000000;
        } else if (ty == FLOAT) {
            ast_NumF(0xbf800000);
            ast_Oper((int)(n + Num_words), MulF);
        } else {
            ast_Num(-1);
            ast_Oper((int)(n + Num_words), Mul);
        }
        if (ty != FLOAT) {
            ty = INT;
        }
        break;
    case Inc:
    case Dec: // processing ++x and --x. x-- and x++ is handled later
        t = tk;
        next();
        expr(Inc);
        if (ty == FLOAT) {
            fatal("no ++/-- on float");
        }
        if (ast_Tk(n) != Load) {
            fatal("bad lvalue in pre-increment");
        }
        ast_Tk(n) = t;
        break;
    case 0:
        fatal("unexpected EOF in expression");
    default:
        if (tk & COMPOUND) {
            tk ^= COMPOUND;
        } else {
            fatal("bad expression");
        }
    }

    // "precedence climbing" or "Top Down Operator Precedence" method
    while (tk >= lev) {
        // tk is ASCII code will not exceed `Num=128`. Its value may be changed
        // during recursion, so back up currently processed expression type
        t = ty;
        b = n;
        switch (tk) {
        case Assign:
            if (t & 3) {
                fatal("Cannot assign to array type lvalue");
            }
            // the left part is processed by the variable part of `tk=ID`
            // and pushes the address
            if (ast_Tk(n) != Load) {
                fatal("bad lvalue in assignment");
            }
            // get the value of the right part `expr` as the result of `a=expr`
            n += Load_words;
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
            if (t & 3) {
                fatal("Cannot assign to array type lvalue");
            }
            if (ast_Tk(n) != Load) {
                fatal("bad lvalue in assignment");
            }
            n += Load_words;
            b = n;
            ast_End();
            ast_Load(t);
            if (tk < ShlAssign) {
                tk = Or + (tk - OrAssign);
            } else {
                tk = Shl + (tk - ShlAssign);
            }
            tk |= COMPOUND;
            ty = t;
            compound = 1;
            expr(Assign);
            ast_Assign((int)b, (ty << 16) | t);
            ty = t;
            return;
        case Cond: // `x?a:b` is similar to if except that it relies on else
            next();
            expr(Assign);
            tc = ty;
            if (tk != ':') {
                fatal("conditional missing colon");
            }
            next();
            c = n;
            expr(Cond);
            if (tc != ty) {
                fatal("both results need same type");
            }
            ast_Cond((int)n, (int)c, (int)b);
            break;
        case Lor: // short circuit, the logical or
            next();
            expr(Lan);
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                Num_entry(b).val = Num_entry(b).val || Num_entry(n).val;
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
                Num_entry(b).val = Num_entry(b).val && Num_entry(n).val;
                n = b;
            } else {
                ast_Oper((int)b, Lan);
            }
            ty = INT;
            break;
        case Or: // push the current value, calculate the right value
            next();
            if (compound) {
                compound = 0;
                expr(Assign);
            } else {
                expr(Xor);
            }
            bitopcheck(t, ty);
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                Num_entry(b).val = Num_entry(b).val | Num_entry(n).val;
                n = b;
            } else {
                ast_Oper((int)b, Or);
            }
            ty = INT;
            break;
        case Xor:
            next();
            if (compound) {
                compound = 0;
                expr(Assign);
            } else {
                expr(And);
            }
            bitopcheck(t, ty);
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                Num_entry(b).val = Num_entry(b).val ^ Num_entry(n).val;
                n = b;
            } else {
                ast_Oper((int)b, Xor);
            }
            ty = INT;
            break;
        case And:
            next();
            if (compound) {
                compound = 0;
                expr(Assign);
            } else {
                expr(Eq);
            }
            bitopcheck(t, ty);
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                Num_entry(b).val = Num_entry(b).val & Num_entry(n).val;
                n = b;
            } else {
                ast_Oper((int)b, And);
            }
            ty = INT;
            break;
        case Eq:
            next();
            expr(Ge);
            typecheck(Eq, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    Num_entry(b).val = Num_entry(n).val == Num_entry(b).val;
                    ast_Tk(b) = Num;
                    n = b;
                } else {
                    ast_Oper((int)b, EqF);
                }
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    Num_entry(b).val = Num_entry(b).val == Num_entry(n).val;
                    n = b;
                } else {
                    ast_Oper((int)b, Eq);
                }
            }
            ty = INT;
            break;
        case Ne:
            next();
            expr(Ge);
            typecheck(Ne, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    Num_entry(b).val = Num_entry(n).val != Num_entry(b).val;
                    ast_Tk(b) = Num;
                    n = b;
                } else {
                    ast_Oper((int)b, NeF);
                }
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    Num_entry(b).val = Num_entry(b).val != Num_entry(n).val;
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
                    Num_entry(b).val =
                        (*((float*)&Num_entry(b).val) >= *((float*)&Num_entry(n).val));
                    ast_Tk(b) = Num;
                    n = b;
                } else {
                    ast_Oper((int)b, GeF);
                }
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    Num_entry(b).val = Num_entry(b).val >= Num_entry(n).val;
                    n = b;
                } else {
                    ast_Oper((int)b, Ge);
                }
            }
            ty = INT;
            break;
        case Lt:
            next();
            expr(Shl);
            typecheck(Lt, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    Num_entry(b).val =
                        (*((float*)&Num_entry(b).val) < *((float*)&Num_entry(n).val));
                    ast_Tk(b) = Num;
                    n = b;
                } else {
                    ast_Oper((int)b, LtF);
                }
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    Num_entry(b).val = Num_entry(b).val < Num_entry(n).val;
                    n = b;
                } else {
                    ast_Oper((int)b, Lt);
                }
            }
            ty = INT;
            break;
        case Gt:
            next();
            expr(Shl);
            typecheck(Gt, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    Num_entry(b).val =
                        (*((float*)&Num_entry(b).val) > *((float*)&Num_entry(n).val));
                    ast_Tk(b) = Num;
                    n = b;
                } else {
                    ast_Oper((int)b, GtF);
                }
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    Num_entry(b).val = Num_entry(b).val > Num_entry(n).val;
                    n = b;
                } else {
                    ast_Oper((int)b, Gt);
                }
            }
            ty = INT;
            break;
        case Le:
            next();
            expr(Shl);
            typecheck(Le, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    Num_entry(b).val =
                        (*((float*)&Num_entry(b).val) <= *((float*)&Num_entry(n).val));
                    ast_Tk(b) = Num;
                    n = b;
                } else {
                    ast_Oper((int)b, LeF);
                }
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    Num_entry(b).val = Num_entry(b).val <= Num_entry(n).val;
                    n = b;
                } else {
                    ast_Oper((int)b, Le);
                }
            }
            ty = INT;
            break;
        case Shl:
            next();
            if (compound) {
                compound = 0;
                expr(Assign);
            } else {
                expr(Add);
            }
            bitopcheck(t, ty);
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                Num_entry(b).val = (Num_entry(n).val < 0) ? Num_entry(b).val >> -Num_entry(n).val
                                                          : Num_entry(b).val << Num_entry(n).val;
                n = b;
            } else {
                ast_Oper((int)b, Shl);
            }
            ty = INT;
            break;
        case Shr:
            next();
            if (compound) {
                compound = 0;
                expr(Assign);
            } else {
                expr(Add);
            }
            bitopcheck(t, ty);
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                Num_entry(b).val = (Num_entry(n).val < 0) ? Num_entry(b).val << -Num_entry(n).val
                                                          : Num_entry(b).val >> Num_entry(n).val;
                n = b;
            } else {
                ast_Oper((int)b, Shr);
            }
            ty = INT;
            break;
        case Add:
            next();
            if (compound) {
                compound = 0;
                expr(Assign);
            } else {
                expr(Mul);
            }
            typecheck(Add, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    *((float*)&Num_entry(b).val) =
                        (*((float*)&Num_entry(b).val) + *((float*)&Num_entry(n).val));
                    n = b;
                } else {
                    ast_Oper((int)b, AddF);
                }
            } else { // both terms are either int or "int *"
                tc = ((t | ty) & (PTR | PTR2)) ? (t >= PTR) : (t >= ty);
                c = n;
                if (tc) {
                    ty = t;
                }
                sz = (ty >= PTR2) ? sizeof(int) : ((ty >= PTR) ? tsize[(ty - PTR) >> 2] : 1);
                if (ast_Tk(n) == Num && tc) {
                    Num_entry(n).val *= sz;
                    sz = 1;
                } else if (ast_Tk(b) == Num && !tc) {
                    Num_entry(b).val *= sz;
                    sz = 1;
                }
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    Num_entry(b).val += Num_entry(n).val;
                    n = b;
                } else if (sz != 1) {
                    ast_Num(sz);
                    ast_Oper((int)(tc ? c : b), Mul);
                    ast_Oper((int)(tc ? b : c), Add);
                } else {
                    ast_Oper((int)b, Add);
                }
            }
            break;
        case Sub:
            next();
            if (compound) {
                compound = 0;
                expr(Assign);
            } else {
                expr(Mul);
            }
            typecheck(Sub, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    *((float*)&Num_entry(b).val) =
                        (*((float*)&Num_entry(b).val) - *((float*)&Num_entry(n).val));
                    n = b;
                } else {
                    ast_Oper((int)b, SubF);
                }
            } else {            // 4 cases: ptr-ptr, ptr-int, int-ptr (err), int-int
                if (t >= PTR) { // left arg is ptr
                    sz = (t >= PTR2) ? sizeof(int) : tsize[(t - PTR) >> 2];
                    if (ty >= PTR) { // ptr - ptr
                        if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                            Num_entry(b).val = (Num_entry(b).val - Num_entry(n).val) / sz;
                            n = b;
                        } else {
                            ast_Oper((int)b, Sub);
                            if (sz > 1) {
                                if (is_power_of_2(sz)) { // 2^n
                                    ast_Num(__builtin_popcount(sz - 1));
                                    ast_Oper((int)(n + Num_words), Shr);
                                } else {
                                    ast_Num(sz);
                                    ast_Oper((int)(n + Num_words), Div);
                                }
                            }
                        }
                        ty = INT;
                    } else { // ptr - int
                        if (ast_Tk(n) == Num) {
                            Num_entry(n).val *= sz;
                            if (ast_Tk(b) == Num) {
                                Num_entry(b).val = Num_entry(b).val - Num_entry(n).val;
                                n = b;
                            } else {
                                ast_Oper((int)b, Sub);
                            }
                        } else {
                            if (sz > 1) {
                                if (is_power_of_2(sz)) { // 2^n
                                    ast_Num(__builtin_popcount(sz - 1));
                                    ast_Oper((int)(n + Num_words), Shl);
                                } else {
                                    ast_Num(sz);
                                    ast_Oper((int)(n + Num_words), Mul);
                                }
                            }
                            ast_Oper((int)b, Sub);
                        }
                        ty = t;
                    }
                } else { // int - int
                    if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                        Num_entry(b).val = Num_entry(b).val - Num_entry(n).val;
                        n = b;
                    } else {
                        ast_Oper((int)b, Sub);
                    }
                    ty = INT;
                }
            }
            break;
        case Mul:
            next();
            if (compound) {
                compound = 0;
                expr(Assign);
            } else {
                expr(Inc);
            }
            typecheck(Mul, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    *((float*)&Num_entry(b).val) *= *((float*)&Num_entry(n).val);
                    n = b;
                } else {
                    ast_Oper((int)b, MulF);
                }
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    Num_entry(b).val *= Num_entry(n).val;
                    n = b;
                } else {
                    if (ast_Tk(n) == Num && Num_entry(n).val > 0 &&
                        is_power_of_2(Num_entry(n).val)) {
                        Num_entry(n).val = __builtin_popcount(Num_entry(n).val - 1);
                        ast_Oper((int)b, Shl); // 2^n
                    } else {
                        ast_Oper((int)b, Mul);
                    }
                }
                ty = INT;
            }
            break;
        case Inc:
        case Dec:
            if (ty & 3) {
                fatal("can't inc/dec an array variable");
            }
            if (ty == FLOAT) {
                fatal("no ++/-- on float");
            }
            sz = (ty >= PTR2) ? sizeof(int) : ((ty >= PTR) ? tsize[(ty - PTR) >> 2] : 1);
            if (ast_Tk(n) != Load) {
                fatal("bad lvalue in post-increment");
            }
            ast_Tk(n) = tk;
            ast_Num(sz);
            ast_Oper((int)b, (tk == Inc) ? Sub : Add);
            next();
            break;
        case Div:
            next();
            if (compound) {
                compound = 0;
                expr(Assign);
            } else {
                expr(Inc);
            }
            typecheck(Div, t, ty);
            if (ty == FLOAT) {
                if (ast_Tk(n) == NumF && ast_Tk(b) == NumF) {
                    *((float*)&Num_entry(b).val) =
                        (*((float*)&Num_entry(b).val) / *((float*)&Num_entry(n).val));
                    n = b;
                } else {
                    ast_Oper((int)b, DivF);
                }
            } else {
                if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                    Num_entry(b).val /= Num_entry(n).val;
                    n = b;
                } else {
                    if (ast_Tk(n) == Num && Num_entry(n).val > 0 &&
                        is_power_of_2(Num_entry(n).val)) {
                        Num_entry(n).val = __builtin_popcount(Num_entry(n).val - 1);
                        ast_Oper((int)b, Shr); // 2^n
                    } else {
                        ast_Oper((int)b, Div);
                    }
                }
                ty = INT;
            }
            break;
        case Mod:
            next();
            if (compound) {
                compound = 0;
                expr(Assign);
            } else {
                expr(Inc);
            }
            typecheck(Mod, t, ty);
            if (ty == FLOAT) {
                fatal("use fmodf() for float modulo");
            }
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                Num_entry(b).val %= Num_entry(n).val;
                n = b;
            } else {
                if (ast_Tk(n) == Num && Num_entry(n).val > 0 && is_power_of_2(Num_entry(n).val)) {
                    --Num_entry(n).val;
                    ast_Oper((int)b, And); // 2^n
                } else {
                    ast_Oper((int)b, Mod);
                }
            }
            ty = INT;
            break;
        case Dot:
            t += PTR;
            if (ast_Tk(n) == Load && Load_entry(n).typ > ATOM_TYPE && Load_entry(n).typ < PTR) {
                n += Load_words; // struct
            }
        case Arrow:
            if (t <= PTR + ATOM_TYPE || t >= PTR2) {
                fatal("structure expected");
            }
            next();
            if (tk != Id) {
                fatal("structure member expected");
            }
            m = members[(t - PTR) >> 2];
            while (m && m->id != id) {
                m = m->next;
            }
            if (!m) {
                fatal("structure member not found");
            }
            if (m->offset) {
                ast_Num(m->offset);
                ast_Oper((int)(n + Num_words), Add);
            }
            ty = m->type;
            next();
            if (!(ty & 3)) {
                ast_Load((ty >= PTR) ? INT : ty);
                break;
            }
            memsub = 1;
            int dim = ty & 3;
            int ee = m->etype;
            b = n;
            t = ty & ~3;
        case Bracket:
            if (t < PTR) {
                fatal("pointer type expected");
            }
            if (memsub == 0) {
                dim = id->type & 3;
                ee = id->etype;
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
                if (ty >= FLOAT) {
                    fatal("non-int array index");
                }
                if (tk != ']') {
                    fatal("close bracket expected");
                }
                c = n;
                next();
                if (dim) {
                    int factor = ((ii == 2) ? (((ee >> 11) & 0x3ff) + 1) : 1);
                    factor *=
                        ((dim == 3 && ii >= 1) ? ((ee & 0x7ff) + 1)
                                               : ((dim == 2 && ii == 1) ? ((ee & 0xffff) + 1) : 1));
                    if (ast_Tk(n) == Num) {
                        // elision with struct offset for efficiency
                        if (ast_Tk(b) == Add && ast_Tk(b + Oper_words) == Num) {
                            Num_entry(b + Oper_words).val += factor * Num_entry(n).val * sz;
                        } else {
                            sum += factor * Num_entry(n).val;
                        }
                        n += Num_words; // delete the subscript constant
                    } else {
                        // generate code to add a term
                        if (factor > 1) {
                            ast_Num(factor);
                            ast_Oper((int)c, Mul);
                        }
                        if (f) {
                            ast_Oper((int)f, Add);
                        }
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
                } else if (!f) {
                    goto add_simple;
                }
            }
            if (sz > 1) {
                if (ast_Tk(n) == Num) {
                    Num_entry(n).val *= sz;
                } else {
                    ast_Num(sz);
                    ast_Oper((int)(n + Num_words), Mul);
                }
            }
            if (ast_Tk(n) == Num && ast_Tk(b) == Num) {
                Num_entry(b).val += Num_entry(n).val;
                n = b;
            } else {
                ast_Oper((int)b, Add);
            }
        add_simple:
            if (doload) {
                ast_Load(((ty = t) >= PTR) ? INT : ty);
            }
            break;
        default:
            fatal("%d: compiler error tk=%d\n", lineno, tk);
        }
    }
}

// global array initialization
static void init_array(struct ident_s* tn, int extent[], int dim) {
    int i, cursor, match, coff = 0, off, empty, *vi;
    int inc[3];

    inc[0] = extent[dim - 1];
    for (i = 1; i < dim; ++i) {
        inc[i] = inc[i - 1] * extent[dim - (i + 1)];
    }

    // Global is preferred to local.
    // Either suggest global or automatically move to global scope.
    if (tn->class != Glo) {
        fatal("only global array initialization supported");
    }

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
            if (cursor) {
                --cursor;
            } else {
                fatal("overly nested initializer");
            }
            empty = 1;
            continue;
        } else if (tk == '}') {
            next();
            // skip remainder elements on this level (or set 0 if cmdline opt)
            if ((off = i % inc[cursor + coff]) || empty) {
                i += (inc[cursor + coff] - off);
            }
            if (++cursor == dim - coff) {
                break;
            }
        } else {
            expr(Cond);
            if (ast_Tk(n) != Num && ast_Tk(n) != NumF) {
                fatal("non-literal initializer");
            }

            if (ty == CHAR + PTR) {
                if (match == CHAR + PTR2) {
                    vi[i++] = Num_entry(n).val;
                } else if (match == CHAR + PTR) {
                    off = strlen((char*)Num_entry(n).val) + 1;
                    if (off > inc[0]) {
                        off = inc[0];
                        printf("%d: string '%s' truncated to %d chars\n", lineno,
                               (char*)Num_entry(n).val, off);
                    }
                    memcpy((char*)vi + i, (char*)Num_entry(n).val, off);
                    i += inc[0];
                } else {
                    fatal("can't assign string to scalar");
                }
            } else if (ty == match) {
                vi[i++] = Num_entry(n).val;
            } else if (ty == INT) {
                if (match == CHAR + PTR) {
                    *((char*)vi + i) = Num_entry(n).val;
                    i += inc[0];
                } else {
                    *((float*)&Num_entry(n).val) = (float)Num_entry(n).val;
                    vi[i++] = Num_entry(n).val;
                }
            } else if (ty == FLOAT) {
                if (match == INT) {
                    vi[i++] = (int)*((float*)(&Num_entry(n).val));
                } else {
                    fatal("illegal char/string initializer");
                }
            }
            n += Num_words; // clean up AST
            empty = 0;
        }
        if (tk == ',') {
            next();
        }
    } while (1);
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
            if (ast_Tk(n) != Num) {
                fatal("non-const array size");
            }
            if (Num_entry(n).val <= 0) {
                fatal("non-positive array dimension");
            }
            if (tk != ']') {
                fatal("missing ]");
            }
            next();
            extent[*dims] = Num_entry(n).val;
            *size *= Num_entry(n).val;
            n += Num_words;
        }
        ++*dims;
    } while (tk == Bracket && *dims < 3);
    if (tk == Bracket) {
        fatal("three subscript max on decl");
    }
    switch (*dims) {
    case 1:
        *et = (extent[0] - 1);
        break;
    case 2:
        *et = ((extent[0] - 1) << 16) + (extent[1] - 1);
        if (extent[0] > 32768 || extent[1] > 65536) {
            fatal("max bounds [32768][65536]");
        }
        break;
    case 3:
        *et = ((extent[0] - 1) << 21) + ((extent[1] - 1) << 11) + (extent[2] - 1);
        if (extent[0] > 1024 || extent[1] > 1024 || extent[2] > 2048) {
            fatal("max bounds [1024][1024][2048]");
        }
        break;
    }
}

static void check_label(int** tt) {
    if (tk != Id) {
        return;
    }
    char* ss = p;
    while (*ss == ' ' || *ss == '\t') {
        ++ss;
    }
    if (*ss == ':') {
        if (id->class != 0 || !(id->type == 0 || id->type == -1)) {
            fatal("invalid label");
        }
        id->type = -1; // hack for id->class deficiency
        ast_Label((int)id);
        ast_Begin(*tt);
        *tt = n;
        next();
        next();
    }
}

// statement parsing (syntax analysis, except for declarations)
void stmt(int ctx) {
    struct ident_s* dd;
    int *a, *b, *c, *d;
    int i, j, nf, atk, sz;
    int nd[3];
    int bt;

    if (ctx == Glo && (tk < Enum || tk > Union)) {
        fatal("syntax: statement used outside function");
    }

    switch (tk) {
    case Enum:
        next();
        // If current token is not "{", it means having enum type name.
        // Skip the enum type name.
        if (tk == Id) {
            next();
        }
        if (tk == '{') {
            next();
            i = 0; // Enum value starts from 0
            while (tk != '}') {
                // Current token should be enum name.
                // If current token is not identifier, stop parsing.
                if (tk != Id) {
                    fatal("bad enum identifier");
                }
                dd = id;
                next();
                if (tk == Assign) {
                    next();
                    expr(Cond);
                    if (ast_Tk(n) != Num) {
                        fatal("bad enum initializer");
                    }
                    i = Num_entry(n).val;
                    n += Num_words; // Set enum value
                }
                dd->class = Num;
                dd->type = INT;
                dd->val = i++;
                if (tk == ',') {
                    next(); // If current token is ",", skip.
                }
            }
            next(); // Skip "}"
        } else if (tk == Id) {
            if (ctx != Par) {
                fatal("enum can only be declared as parameter");
            }
            id->type = INT;
            id->class = ctx;
            id->val = ld++;
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
                if (!id->type) {
                    id->type = tnew++ << 2;
                }
                bt = id->type;
                next();
            } else {
                bt = tnew++ << 2;
            }
            if (tk == '{') {
                next();
                if (members[bt >> 2]) {
                    fatal("duplicate structure definition");
                }
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
                        if (tk != Id || id->type <= ATOM_TYPE || id->type >= PTR) {
                            fatal("bad struct/union declaration");
                        }
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
                        if (tk != Id) {
                            fatal("bad struct member definition");
                        }
                        sz = (ty >= PTR) ? sizeof(int) : tsize[ty >> 2];
                        struct member_s* m = cc_malloc(sizeof(struct member_s), 1);
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
                            if (i > tsize[bt >> 2]) {
                                tsize[bt >> 2] = i;
                            }
                            i = 0;
                        }
                        if (tk == ',') {
                            next();
                        }
                    }
                    next();
                }
                next();
                if (atk != Union) {
                    tsize[bt >> 2] = i;
                }
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
                if (tk != Id) {
                    fatal("bad global declaration");
                }
                if (id->class >= ctx) {
                    fatal("duplicate global definition");
                }
                break;
            case Loc:
                if (tk != Id) {
                    fatal("bad local declaration");
                }
                if (id->class >= ctx) {
                    fatal("duplicate local definition");
                }
                break;
            }
            next();
            if (tk == '(') {
                rtf = 0;
                rtt = (ty == 0 && !memcmp(dd->name, "void", 4)) ? -1 : ty;
            }
            dd = id;
            if (dd->forward && (dd->type != ty)) {
                fatal("Function return type does not match prototype");
            }
            dd->type = ty;
            if (tk == '(') { // function
                if (b != 0) {
                    fatal("func decl can't be mixed with var decl(s)");
                }
                if (ctx != Glo) {
                    fatal("nested function");
                }
                if (ty > ATOM_TYPE && ty < PTR) {
                    fatal("return type can't be struct");
                }
                if (id->class == Func && id->val > (int)text_base && id->val < (int)e &&
                    id->forward == 0) {
                    fatal("duplicate global definition");
                }
                int ddetype = 0;
                dd->class = Func;       // type is function
                dd->val = (int)(e + 1); // function Pointer? offset/address
                next();
                nf = ld = 0; // "ld" is parameter's index.
                while (tk != ')') {
                    stmt(Par);
                    ddetype = ddetype * 2;
                    if (ty == FLOAT) {
                        ++nf;
                        ++ddetype;
                    }
                    if (tk == ',') {
                        next();
                    }
                }
                if (ld > ADJ_MASK) {
                    fatal("maximum of %d function parameters", ADJ_MASK);
                }
                // function etype is not like other etypes
                next();
                ddetype = (ddetype << 10) + (nf << 5) + ld; // prm info
                if (dd->forward && (ddetype != dd->etype)) {
                    fatal("parameters don't match prototype");
                }
                dd->etype = ddetype;
                uint16_t* se;
                if (tk == ';') { // check for prototype
                    se = e;
                    if (!((int)e & 2)) {
                        emit(0x46c0); // nop
                    }
                    emit(0x4800); // ldr r0, [pc, #0]
                    emit(0xe001); // b.n 1
                    dd->forward = e;
                    emit_word(0);
                    emit(0x4700); // bx  r0
                } else {          // function with body
                    if (tk != '{') {
                        fatal("bad function definition");
                    }
                    loc = ++ld;
                    if (dd->forward) {
                        uint16_t* te = e;
                        e = dd->forward;
                        emit_word(dd->val | 1);
                        e = te;
                        dd->forward = 0;
                    }
                    next();
                    // Not declaration and must not be function, analyze inner block.
                    // e represents the address which will store pc
                    // (ld - loc) indicates memory size to allocate
                    ast_End();
                    while (tk != '}') {
                        int* t = n;
                        check_label(&t);
                        stmt(Loc);
                        if (t != n) {
                            ast_Begin(t);
                        }
                    }
                    if (rtf == 0 && rtt != -1) {
                        fatal("expecting return value");
                    }
                    ast_Enter(ld - loc);
                    ncas = 0;
                    se = e;
                    gen(n);
                }
                if (src_opt) {
                    printf("%d: %.*s\n", lineno, p - lp, lp);
                    lp = p;
                    disasm_address(&state, (int)(se + 1));
                    while (state.address < (int)e - 1) {
                        uint16_t* nxt = (uint16_t*)(state.address + state.size);
                        disasm_thumb(&state, *nxt, *(nxt + 1));
                        printf("%s\n", state.text);
                    }
                }
                id = sym_base;
                struct ident_s* id2 = (struct ident_s*)&sym_base;
                while (id) { // unwind symbol table locals
                    if (id->class == Loc || id->class == Par) {
                        id->class = id->hclass;
                        id->type = id->htype;
                        id->val = id->hval;
                        id->etype = id->hetype;
                        id2 = id;
                        id = id->next;
                    } else if (id->class == Label) { // clear id for next func
                        struct ident_s* id3 = id;
                        id = id->next;
                        cc_free(id3);
                        id2->next = id;
                    } else if (id->class == 0 && id->type == -1) {
                        fatal("%d: label %.*s not defined\n", lineno, id->hash & 0x3f, id->name);
                    } else {
                        id2 = id;
                        id = id->next;
                    }
                }
            } else {
                if (ty > ATOM_TYPE && ty < PTR && tsize[bt >> 2] == 0) {
                    fatal("struct/union forward declaration is unsupported");
                }
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
                    if (sz > 1) {
                        data = (char*)(((int)data + 3) & ~3);
                    }
                    if (src_opt && !dd->inserted) {
                        int len = dd->hash & 0x3f;
                        char ch = dd->name[len];
                        dd->name[len] = 0;
                        disasm_symbol(&state, dd->name, (int)data, ARMMODE_THUMB);
                        dd->name[len] = ch;
                    }
                    dd->val = (int)data;
                    if ((data + sz) > (data_base + DATA_BYTES)) {
                        fatal("program data exceeds data segment");
                    }
                    data += sz;
                } else if (ctx == Loc) {
                    dd->val = (ld += (sz + 3) / sizeof(int));
                } else if (ctx == Par) {
                    if (ty > ATOM_TYPE && ty < PTR) { // local struct decl
                        fatal("struct parameters must be pointers");
                    }
                    dd->val = ld++;
                }
                if (tk == Assign) {
                    next();
                    if (ctx == Par) {
                        fatal("default arguments not supported");
                    }
                    if (tk == '{' && (dd->type & 3)) {
                        init_array(dd, nd, j);
                    } else {
                        if (ctx == Loc) {
                            if (b == 0) {
                                ast_End();
                            }
                            b = n;
                            ast_Loc(loc - dd->val);
                            a = n;
                            i = ty;
                            expr(Assign);
                            typecheck(Assign, i, ty);
                            ast_Assign((int)a, (ty << 16) | i);
                            ty = i;
                            ast_Begin(b);
                        } else { // ctx == Glo
                            i = ty;
                            expr(Cond);
                            typecheck(Assign, i, ty);
                            if (ast_Tk(n) != Num && ast_Tk(n) != NumF) {
                                fatal("global assignment must eval to lit expr");
                            }
                            if (ty == CHAR + PTR && (dd->type & 3) != 1) {
                                fatal("use decl char foo[nn] = \"...\";");
                            }
                            if ((ast_Tk(n) == Num && (i == CHAR || i == INT)) ||
                                (ast_Tk(n) == NumF && i == FLOAT)) {
                                *((int*)dd->val) = Num_entry(n).val;
                            } else if (ty == CHAR + PTR) {
                                i = strlen((char*)Num_entry(n).val) + 1;
                                if (i > (dd->etype + 1)) {
                                    i = dd->etype + 1;
                                    printf("%d: string truncated to width\n", lineno);
                                }
                                memcpy((char*)dd->val, (char*)Num_entry(n).val, i);
                            } else {
                                fatal("unsupported global initializer");
                            }
                            n += Num_words;
                        }
                    }
                }
            }
            if (ctx != Par && tk == ',') {
                next();
            }
        }
        return;
    case If:
        next();
        if (tk != '(') {
            fatal("open parenthesis expected");
        }
        next();
        expr(Assign);
        a = n;
        if (tk != ')') {
            fatal("close parenthesis expected");
        }
        next();
        stmt(ctx);
        b = n;
        if (tk == Else) {
            next();
            stmt(ctx);
            d = n;
        } else {
            d = 0;
        }
        ast_Cond((int)d, (int)b, (int)a);
        return;
    case While:
        next();
        if (tk != '(') {
            fatal("open parenthesis expected");
        }
        next();
        expr(Assign);
        b = n; // condition
        if (tk != ')') {
            fatal("close parenthesis expected");
        }
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
        if (tk != While) {
            fatal("while expected");
        }
        next();
        if (tk != '(') {
            fatal("open parenthesis expected");
        }
        next();
        ast_End();
        expr(Assign);
        b = n;
        if (tk != ')') {
            fatal("close parenthesis expected");
        }
        next();
        ast_While((int)b, (int)a, DoWhile);
        return;
    case Switch:
        i = 0;
        j = (int)ncas;
        ncas = &i;
        next();
        if (tk != '(') {
            fatal("open parenthesis expected");
        }
        next();
        expr(Assign);
        a = n;
        if (tk != ')') {
            fatal("close parenthesis expected");
        }
        next();
        ++swtc;
        ++brkc;
        stmt(ctx);
        --swtc;
        --brkc;
        b = n;
        ast_Switch((int)b, (int)a);
        ncas = (int*)j;
        return;
    case Case:
        if (!swtc) {
            fatal("case-statement outside of switch");
        }
        i = *ncas;
        next();
        expr(Or);
        a = n;
        if (ast_Tk(n) != Num) {
            fatal("case label not a numeric literal");
        }
        j = Num_entry(n).val;
        // Num_entry(n).val;
        *ncas = j;
        ast_End();
        if (tk != ':') {
            fatal("colon expected");
        }
        next();
        stmt(ctx);
        b = n;
        ast_Case((int)b, (int)a);
        return;
    case Break:
        if (!brkc) {
            fatal("misplaced break statement");
        }
        next();
        if (tk != ';') {
            fatal("semicolon expected");
        }
        next();
        ast_Single(Break);
        return;
    case Continue:
        if (!cntc) {
            fatal("misplaced continue statement");
        }
        next();
        if (tk != ';') {
            fatal("semicolon expected");
        }
        next();
        ast_Single(Continue);
        return;
    case Default:
        if (!swtc) {
            fatal("default-statement outside of switch");
        }
        next();
        if (tk != ':') {
            fatal("colon expected");
        }
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
            if (rtt == -1) {
                fatal("not expecting return value");
            }
            typecheck(Eq, rtt, ty);
        } else {
            if (rtt != -1) {
                fatal("return value expected");
            }
        }
        rtf = 1; // signal a return statement exisits
        ast_Return((int)a);
        if (tk != ';') {
            fatal("semicolon expected");
        }
        next();
        return;
    /* For iteration is implemented as:
     * Init -> Cond -> Bz to end -> Jmp to Body
     * After -> Jmp to Cond -> Body -> Jmp to After
     */
    case For:
        next();
        if (tk != '(') {
            fatal("open parenthesis expected");
        }
        next();
        ast_End();
        if (tk != ';') {
            expr(Assign);
        }
        while (tk == ',') {
            int* f = n;
            next();
            expr(Assign);
            ast_Begin(f);
        }
        d = n;
        if (tk != ';') {
            fatal("semicolon expected");
        }
        next();
        ast_End();
        if (tk != ';') {
            expr(Assign);
            a = n; // Point to entry of for cond
            if (tk != ';') {
                fatal("semicolon expected");
            }
        } else {
            a = 0;
        }
        next();
        ast_End();
        if (tk != ')') {
            expr(Assign);
        }
        while (tk == ',') {
            int* g = n;
            next();
            expr(Assign);
            ast_Begin(g);
        }
        b = n;
        if (tk != ')') {
            fatal("close parenthesis expected");
        }
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
        if (tk != Id || (id->type != 0 && id->type != -1) ||
            (id->class != Label && id->class != 0)) {
            fatal("goto expects label");
        }
        id->type = -1; // hack for id->class deficiency
        ast_Goto((int)id);
        next();
        if (tk != ';') {
            fatal("semicolon expected");
        }
        next();
        return;
    // stmt -> '{' stmt '}'
    case '{':
        next();
        ast_End();
        while (tk != '}') {
            a = n;
            check_label(&a);
            stmt(ctx);
            if (a != n) {
                ast_Begin(a);
            }
        }
        next();
        return;
    // stmt -> ';'
    case ';':
        next();
        ast_End();
        return;
    default:
        expr(Assign);
        if (tk != ';' && tk != ',') {
            fatal("semicolon expected");
        }
        next();
    }
}

static float i_as_f(int i) {
    union {
        int i;
        float f;
    } u;
    u.i = i;
    return u.f;
}

static int f_as_i(float f) {
    union {
        int i;
        float f;
    } u;
    u.f = f;
    return u.i;
}
