#include "file_cmds.h"
#include <stdio.h>
#include "io.h"
#include "main.h"

static int check_from_to_parms(char** from, char** to, int copy) {
    *from = NULL;
    *to = NULL;
    int rc = 1;
    do {
        if (sh_argc < 3) {
            strcpy(sh_message, "need two names");
            break;
        }
        bool from_is_dir = false;
        bool to_is_dir = false;
        bool to_exists = false;
        *from = strdup(full_path(sh_argv[1]));
        if (*from == NULL) {
            strcpy(sh_message, "no memory");
            break;
        }
        struct lfs_info info;
        if (fs_stat(*from, &info) < LFS_ERR_OK) {
            sprintf(sh_message, "%s not found", *from);
            break;
        }
        from_is_dir = info.type == LFS_TYPE_DIR;
        *to = strdup(full_path(sh_argv[2]));
        if (*to == NULL) {
            strcpy(sh_message, "no memory");
            break;
        }
        if (fs_stat(*to, &info) == LFS_ERR_OK) {
            to_is_dir = info.type == LFS_TYPE_DIR;
            to_exists = 1;
        }
        if (copy && from_is_dir) {
            strcpy(sh_message, "can't copy a directory");
            break;
        }
        if (to_exists && to_is_dir) {
            char* name = strrchr(*from, '/') + 1;
            bool append_slash = (*to)[strlen(*to) - 1] == '/' ? false : true;
            int l = strlen(*to) + strlen(name) + 1;
            if (append_slash) {
                l++;
            }
            char* to2 = malloc(l);
            if (!to2) {
                strcpy(sh_message, "no memory");
                break;
            }
            strcpy(to2, *to);
            if (append_slash) {
                strcat(to2, "/");
            }
            strcat(to2, name);
            free(*to);
            *to = to2;
        }
        rc = 0;
    } while (0);
    if (rc) {
        if (*from) {
            free(*from);
        }
        if (*to) {
            free(*to);
        }
    }
    return rc;
}

uint8_t mv_cmd(void) {
    char* from;
    char* to;
    if (check_from_to_parms(&from, &to, 0)) {
        return 1;
    }
    struct lfs_info info;
    uint8_t ret;
    if (fs_rename(from, to) < LFS_ERR_OK) {
        sprintf(sh_message, "could not move %s to %s", from, to);
        ret = 2;
    } else {
        sprintf(sh_message, "%s moved to %s", from, to);
        ret = 0;
    }
    free(from);
    free(to);
    return ret;
}

uint8_t cp_cmd(void) {
    char* from;
    char* to;
    char* buf = NULL;
    if (check_from_to_parms(&from, &to, 1)) {
        return 1;
    }
    lfs_file_t in, out;
    bool in_ok = false, out_ok = false;
    do {
        buf = malloc(4096);
        if (buf == NULL) {
            strcpy(sh_message, "no memory");
            break;
        }
        if (fs_file_open(&in, from, LFS_O_RDONLY) < LFS_ERR_OK) {
            sprintf(sh_message, "error opening %s", from);
            break;
        }
        in_ok = true;
        if (fs_file_open(&out, to, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) < LFS_ERR_OK) {
            sprintf(sh_message, "error opening %s", from);
            break;
        }
        out_ok = true;
        int l = fs_file_read(&in, buf, 4096);
        while (l > 0) {
            if (fs_file_write(&out, buf, l) != l) {
                sprintf(sh_message, "error writing %s", to);
                break;
            }
            l = fs_file_read(&in, buf, 4096);
        }
    } while (false);
    if (in_ok) {
        fs_file_close(&in);
    }
    if (out_ok) {
        fs_file_close(&out);
    }
    if (buf) {
        if (out_ok && fs_getattr(from, 1, buf, 4) == 4 && strcmp(buf, "exe") == 0) {
            fs_setattr(to, 1, buf, 4);
        }
        free(buf);
    }
    uint8_t ret;
    if (!sh_message[0]) {
        sprintf(sh_message, "file %s copied to %s", from, to);
        ret = 0;
    } else {
        ret = 1;
    }
    free(from);
    free(to);
    return ret;
}

uint8_t mkdir_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    if (bad_name()) {
        return 2;
    }
    if (fs_mkdir(full_path(sh_argv[1])) < LFS_ERR_OK) {
        strcpy(sh_message, "Can't create directory");
        return 3;
    }
    sprintf(sh_message, "%s created", full_path(sh_argv[1]));
    return 0;
}

static char rmdir_path[256];

static bool clean_dir(char* name) {
    int path_len = strlen(rmdir_path);
    if (path_len) {
        strcat(rmdir_path, "/");
    }
    strcat(rmdir_path, name);
    lfs_dir_t dir_f;
    if (fs_dir_open(&dir_f, rmdir_path) < LFS_ERR_OK) {
        printf("can't open %s directory\n", rmdir_path);
        return false;
    }
    struct lfs_info info;
    while (fs_dir_read(&dir_f, &info) > 0) {
        if (info.type == LFS_TYPE_DIR && strcmp(info.name, ".") && strcmp(info.name, "..")) {
            if (!clean_dir(info.name)) {
                fs_dir_close(&dir_f);
                return false;
            }
        }
    }
    fs_dir_rewind(&dir_f);
    while (fs_dir_read(&dir_f, &info) > 0) {
        if (info.type == LFS_TYPE_REG) {
            int plen = strlen(rmdir_path);
            strcat(rmdir_path, "/");
            strcat(rmdir_path, info.name);
            if (fs_remove(rmdir_path) < LFS_ERR_OK) {
                printf("can't remove %s", rmdir_path);
                fs_dir_close(&dir_f);
                return false;
            }
            printf("%s removed\n", rmdir_path);
            rmdir_path[plen] = 0;
        }
    }
    fs_dir_close(&dir_f);
    if (fs_remove(rmdir_path) < LFS_ERR_OK) {
        sprintf(sh_message, "can't remove %s", rmdir_path);
        return false;
    }
    printf("%s removed\n", rmdir_path);
    rmdir_path[path_len] = 0;
    return true;
}

uint8_t rm_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    if (bad_name()) {
        return 2;
    }
    bool recursive = false;
    if (strcmp(sh_argv[1], "-r") == 0) {
        if (sh_argc < 3) {
            strcpy(sh_message, "specify a file or directory name");
            return 3;
        }
        recursive = true;
        sh_argv[1] = sh_argv[2];
    }
    // lfs won't remove a non empty directory but returns without error!
    struct lfs_info info;
    char* fp = full_path(sh_argv[1]);
    if (fs_stat(fp, &info) < LFS_ERR_OK) {
        sprintf(sh_message, "%s not found", full_path(sh_argv[1]));
        return 4;
    }
    int isdir = 0;
    if (info.type == LFS_TYPE_DIR) {
        isdir = 1;
        lfs_dir_t dir;
        fs_dir_open(&dir, fp);
        int n = 0;
        while (fs_dir_read(&dir, &info)) {
            if ((strcmp(info.name, ".") != 0) && (strcmp(info.name, "..") != 0)) {
                n++;
            }
        }
        fs_dir_close(&dir);
        if (n) {
            if (recursive) {
                rmdir_path[0] = 0;
                if (!clean_dir(fp)) {
                    return 5;
                } else {
                    return 0;
                }
            } else {
                sprintf(sh_message, "directory %s not empty", fp);
                return 6;
            }
        }
    }
    if (fs_remove(fp) < LFS_ERR_OK) {
        strcpy(sh_message, "Can't remove file or directory");
        return 7;
    } else {
        sprintf(sh_message, "%s %s removed", isdir ? "directory" : "file", fp);
        return 0;
    }
}

uint8_t cat_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    if (bad_name()) {
        return 2;
    }
    lfs_file_t file;
    if (fs_file_open(&file, full_path(sh_argv[1]), LFS_O_RDONLY) < LFS_ERR_OK) {
        strcpy(sh_message, "error opening file");
        return 3;
    }
    int l = fs_file_seek(&file, 0, LFS_SEEK_END);
    fs_file_seek(&file, 0, LFS_SEEK_SET);
    char buf[256];
    uint8_t ret = 0;
    while (l) {
        int l2 = l;
        if (l2 > sizeof(buf)) {
            l2 = sizeof(buf);
        }
        if (fs_file_read(&file, buf, l2) != l2) {
            sprintf(sh_message, "error reading file");
            ret = 4;
            break;
        }
        for (int i = 0; i < l2; ++i) {
            putchar(buf[i]);
        }
        l -= l2;
    }
    fs_file_close(&file);
    return ret;
}

uint8_t ls_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    int show_all = 0;
    char** av = sh_argv;
    if ((sh_argc > 1) && (strcmp(av[1], "-a") == 0)) {
        sh_argc--;
        av++;
        show_all = 1;
    }
    if (sh_argc > 1) {
        full_path(av[1]);
    } else {
        full_path("");
    }
    lfs_dir_t dir;
    if (fs_dir_open(&dir, path) < LFS_ERR_OK) {
        strcpy(sh_message, "not a directory");
        return 2;
    }
    printf("\n");
    struct lfs_info info;
    while (fs_dir_read(&dir, &info) > 0) {
        if (strcmp(info.name, ".") && strcmp(info.name, "..")) {
            if (info.type == LFS_TYPE_DIR) {
                if ((info.name[0] != '.') || show_all) {
                    printf(" %7d %s/\n", info.size, info.name);
                }
            }
        }
    }
    fs_dir_rewind(&dir);
    while (fs_dir_read(&dir, &info) > 0) {
        if (strcmp(info.name, ".") && strcmp(info.name, "..")) {
            if (info.type == LFS_TYPE_REG) {
                if ((info.name[0] != '.') || show_all) {
                    printf(" %7d %s\n", info.size, info.name);
                }
            }
        }
    }
    fs_dir_close(&dir);
    return 0;
}
