#ifndef _CC_INTERNALS_H_
#define _CC_INTERNALS_H_

#include "cc.h"
#include <stdbool.h>
#include "io.h"

#define UDATA __attribute__((section(".ccudata")))

// Number of bits for parameter count
#define ADJ_BITS 5
#define ADJ_MASK ((1 << ADJ_BITS) - 1)

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

// fatal erro message and exit
#define fatal(fmt, ...) fatal_func(__FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)

__attribute__((__noreturn__))
void fatal_func(const char* func, int lne, const char* fmt, ...);

__attribute__((__noreturn__))
void run_fatal(const char* fmt, ...);

#endif
