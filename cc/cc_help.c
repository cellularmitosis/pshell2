#include "cc_help.h"
#include "cc_internals.h"
#include "../pshell/main.h"
#include <stdio.h>

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

static void show_externals(int i) {
    printf("Functions:\n\n");
    int x = term_cols;
    int y = term_rows;
    int pos = 0;
    for (int j = 0; j < numof_externs(); j++) {
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

void cc_help(char* lib) {
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
