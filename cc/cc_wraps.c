#include "cc_wraps.h"
#include <stdio.h>
#include <fcntl.h>
#include "cc_internals.h"
#include "cc_malloc.h"
#include  "../pshell/main.h"
#include "pico/sync.h"

// user malloc shim
void* wrap_malloc(int len) {
    return cc_malloc(len, 0);
};

void* wrap_calloc(int nmemb, int siz) {
    return cc_malloc(nmemb * siz, 1);
};

// user function shims
int wrap_open(char* name, int mode) {
    struct file_handle* h = cc_malloc(sizeof(struct file_handle), 1);
    h->is_dir = false;
    int lfs_mode = (mode & 0xf) + 1;
    if (mode & O_CREAT) {
        lfs_mode |= LFS_O_CREAT;
    }
    if (mode & O_EXCL) {
        lfs_mode |= LFS_O_EXCL;
    }
    if (mode & O_TRUNC) {
        lfs_mode |= LFS_O_TRUNC;
    }
    if (mode & O_APPEND) {
        lfs_mode |= LFS_O_APPEND;
    }
    if (fs_file_open(&h->u.file, full_path(name), lfs_mode) < LFS_ERR_OK) {
        cc_free(h);
        return 0;
    }
    h->next = file_list;
    file_list = h;
    return (int)h;
}

int wrap_opendir(char* name) {
    struct file_handle* h = cc_malloc(sizeof(struct file_handle), 1);
    h->is_dir = true;
    if (fs_dir_open(&h->u.dir, full_path(name)) < LFS_ERR_OK) {
        cc_free(h);
        return 0;
    }
    h->next = file_list;
    file_list = h;
    return (int)h;
}

void wrap_close(int handle) {
    struct file_handle* last_h = (void*)&file_list;
    struct file_handle* h = file_list;
    while (h) {
        if (h == (struct file_handle*)handle) {
            last_h->next = h->next;
            if (h->is_dir) {
                fs_dir_close(&h->u.dir);
            } else {
                fs_file_close(&h->u.file);
            }
            cc_free(h);
            return;
        }
        last_h = h;
        h = h->next;
    }
    run_fatal("closing unopened file!");
}

int wrap_read(int handle, void* buf, int len) {
    struct file_handle* h = (struct file_handle*)handle;
    if (h->is_dir) {
        fatal("use readdir to read from directories");
    }
    return fs_file_read(&h->u.file, buf, len);
}

int wrap_readdir(int handle, void* buf) {
    struct file_handle* h = (struct file_handle*)handle;
    if (!h->is_dir) {
        fatal("use read to read from files");
    }
    return fs_dir_read(&h->u.dir, buf);
}

int wrap_write(int handle, void* buf, int len) {
    struct file_handle* h = (struct file_handle*)handle;
    return fs_file_write(&h->u.file, buf, len);
}

int wrap_lseek(int handle, int pos, int set) {
    struct file_handle* h = (struct file_handle*)handle;
    return fs_file_seek(&h->u.file, pos, set);
};

int wrap_popcount(int n) {
    return __builtin_popcount(n);
};

int wrap_printf(void) {};
int wrap_sprintf(void) {};

int wrap_remove(char* name) {
    return fs_remove(full_path(name));
};

int wrap_rename(char* old, char* new) {
    char* fp = full_path(old);
    char* fpa = cc_malloc(strlen(fp) + 1, 1);
    strcpy(fpa, fp);
    char* fpb = full_path(new);
    int r = fs_rename(fpa, fpb);
    cc_free(fpa);
    return r;
}

int wrap_screen_height(void) {
    return term_rows;
}

int wrap_screen_width(void) {
    return term_cols;
}

void wrap_wfi(void) {
    __wfi();
};

// More shims

// printf/sprintf support

static int common_vfunc(int etype, int prntf, int* sp) {
    int stack[ADJ_MASK + ADJ_MASK + 2];
    int stkp = 0;
    int n_parms = (etype & ADJ_MASK);
    etype >>= 10;
    for (int j = n_parms - 1; j >= 0; j--) {
        if ((etype & (1 << j)) == 0) {
            stack[stkp++] = sp[j];
        } else {
            if (stkp & 1) {
                stack[stkp++] = 0;
            }
            union {
                double d;
                int ii[2];
            } u;
            u.d = *((float*)&sp[j]);
            stack[stkp++] = u.ii[0];
            stack[stkp++] = u.ii[1];
        }
    }
    int r = cc_printf(stack, stkp, prntf);
    if (prntf) {
        fflush(stdout);
    }
    return r;
}

char* x_strdup(char* s) {
    int l = strlen(s);
    char* c = cc_malloc(l + 1, 0);
    strcpy(c, s);
    return c;
}

int x_printf(int etype) {
    int* sp;
    asm volatile("mov %0, sp \n" : "=r"(sp));
    sp += 2;
    common_vfunc(etype, 1, sp);
}

int x_sprintf(int etype) {
    int* sp;
    asm volatile("mov %0, sp \n" : "=r"(sp));
    sp += 2;
    common_vfunc(etype, 0, sp);
}
