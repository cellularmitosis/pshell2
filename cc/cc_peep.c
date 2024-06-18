#include "cc_peep.h"
#include "cc_internals.h"

// peep hole optimizer

// FROM:		   	  TO:
// mov  r0, r7		  mov  r3,r7
// push {r0}		  movs r0,#n
// movs r0,#n
// pop  {r3}

static uint16_t pat0[] = {0x4638, 0xb401, 0x2000, 0xbc08};
static uint16_t msk0[] = {0xffff, 0xffff, 0xff00, 0xffff};
static uint16_t rep0[] = {0x463b, 0x2000};

// ldr  r0,[r0,#n0]   ldr  r3,[r0,#n0]
// push {r0}		  movs r0,#n1
// movs r0, #n1
// pop  {r3}

static uint16_t pat1[] = {0x6800, 0xb401, 0x2000, 0xbc08};
static uint16_t msk1[] = {0xff00, 0xffff, 0xff00, 0xffff};
static uint16_t rep1[] = {0x6803, 0x2000};

// movs r0,#n         mov  r0,r7
// rsbs r0,r0		  subs r0,#n
// add  r0,r7

static uint16_t pat2[] = {0x2000, 0x4240, 0x4438};
static uint16_t msk2[] = {0xff00, 0xffff, 0xffff};
static uint16_t rep2[] = {0x4638, 0x3800};

// push {r0}
// pop {r0}

static uint16_t pat3[] = {0xb401, 0xbc01};
static uint16_t msk3[] = {0xffff, 0xffff};
static uint16_t rep3[0] = {};

// movs r0,#n          mov r1,#n
// push {r0}
// pop  {r1}

static uint16_t pat4[] = {0x2000, 0xb401, 0xbc02};
static uint16_t msk4[] = {0xff00, 0xffff, 0xffff};
static uint16_t rep4[] = {0x2100};

// mov  r0,r7          mov  r3,r7
// subs r0,#n0         subs r3,#n0
// push {r0}           movs r0,#n1
// movs r0,#n1
// pop  {r3}

static uint16_t pat5[] = {0x4638, 0x3800, 0xb401, 0x2000, 0xbc08};
static uint16_t msk5[] = {0xffff, 0xff00, 0xffff, 0xff00, 0xffff};
static uint16_t rep5[] = {0x463b, 0x3b00, 0x2000};

// mov  r0,r7          ldr  r0,[r7,#0]
// ldr  r0,[r0,#0]

static uint16_t pat6[] = {0x4638, 0x6800};
static uint16_t msk6[] = {0xffff, 0xffff};
static uint16_t rep6[] = {0x6838};

// movs r0,#4			lsls r0,r3,#2
// muls r0,r3

static uint16_t pat7[] = {0x2004, 0x4358};
static uint16_t msk7[] = {0xffff, 0xffff};
static uint16_t rep7[] = {0x0098};

// mov  r0,r7          subs r3,r7,#4
// subs r0,#4          movs r0,#n1
// push {r0}
// movs r0,#n1
// pop  {r3}

static uint16_t pat8[] = {0x4638, 0x3804, 0xb401, 0x2000, 0xbc08};
static uint16_t msk8[] = {0xffff, 0xffff, 0xffff, 0xff00, 0xffff};
static uint16_t rep8[] = {0x1f3b, 0x2000};

// mov  r0, r7         sub r0,r7,#4
// subs r0, #4

static uint16_t pat9[] = {0x4638, 0x3804};
static uint16_t msk9[] = {0xffff, 0xffff};
static uint16_t rep9[] = {0x1f38};

// push {r0}            mov  r1,r0
// movs r0,#n           movs r0,#n
// pop  {r1}

static uint16_t pat10[] = {0xb401, 0x2000, 0xbc02};
static uint16_t msk10[] = {0xffff, 0xff00, 0xffff};
static uint16_t rep10[] = {0x4601, 0x2000};

// push {r0}            mov r1,r0
// pop  {r1}

static uint16_t pat11[] = {0xb401, 0xbc02};
static uint16_t msk11[] = {0xffff, 0xffff};
static uint16_t rep11[] = {0x4601};

// movs r0,#n           ldr r0,[r7,#n]
// add  r0,r7
// ldr  r0,[r0,#0]

static uint16_t pat12[] = {0x2000, 0x4438, 0x6800};
static uint16_t msk12[] = {0xff83, 0xffff, 0xffff};
static uint16_t rep12[] = {0x6838};

struct subs {
    int8_t from;
    int8_t to;
    int8_t lshft;
};

#define numof(a) (sizeof(a) / sizeof(a[0]))

static const struct segs {
    uint8_t n_pats;
    uint8_t n_reps;
    uint16_t* pat;
    uint16_t* msk;
    uint16_t* rep;
    struct subs map[2];
} segments[] = {{numof(pat0), numof(rep0), pat0, msk0, rep0, {{2, 1, 0}, {-1, -1, 0}}},
                {numof(pat1), numof(rep1), pat1, msk1, rep1, {{0, 0, 0}, {2, 1, 0}}},
                {numof(pat2), numof(rep2), pat2, msk2, rep2, {{0, 1, 0}, {-1, -1, 0}}},
                {numof(pat3), numof(rep3), pat3, msk3, rep3, {{-1, -1, 0}, {-1, -1, 0}}},
                {numof(pat4), numof(rep4), pat4, msk4, rep4, {{0, 0, 0}, {-1, -1, 0}}},
                {numof(pat8), numof(rep8), pat8, msk8, rep8, {{3, 1, 0}, {-1, -1, 0}}},
                {numof(pat5), numof(rep5), pat5, msk5, rep5, {{1, 1, 0}, {3, 2, 0}}},
                {numof(pat6), numof(rep6), pat6, msk6, rep6, {{-1, -1, 0}, {-1, -1, 0}}},
                {numof(pat7), numof(rep7), pat7, msk7, rep7, {{-1, -1, 0}, {-1, -1, 0}}},
                {numof(pat9), numof(rep9), pat9, msk9, rep9, {{-1, -1, 0}, {-1, -1, 0}}},
                {numof(pat10), numof(rep10), pat10, msk10, rep10, {{1, 1, 0}, {-1, -1, 0}}},
                {numof(pat11), numof(rep11), pat11, msk11, rep11, {{-1, -1, 0}, {-1, -1, 0}}},
                {numof(pat12), numof(rep12), pat12, msk12, rep12, {{0, 0, 4}, {-1, -1, 0}}}};

static int peep_hole(const struct segs* s) {
    uint16_t rslt[8];
    int l = s->n_pats;
    uint16_t* pe = (e - l) + 1;
    if (pe < text_base) {
        return 0;
    }
    for (int i = 0; i < l; i++) {
        rslt[i] = pe[i] & ~s->msk[i];
        if ((pe[i] & s->msk[i]) != s->pat[i]) {
            return 0;
        }
    }
    e -= l;
    l = s->n_reps;
    for (int i = 0; i < l; i++) {
        pe[i] = s->rep[i];
    }
    for (int i = 0; i < 2; ++i) {
        if (s->map[i].from < 0) {
            break;
        }
        pe[s->map[i].to] |= rslt[s->map[i].from] << s->map[i].lshft;
    }
    e += l;
    return 1;
}

void peep(void) {
restart:
    for (int i = 0; i < numof(segments); ++i) {
        if (peep_hole(&segments[i])) {
            goto restart;
        }
    }
}
