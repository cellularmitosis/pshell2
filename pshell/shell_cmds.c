#include "shell_cmds.h"
#include "main.h"
#include "vi.h"
#include "pico/version.h"
#include "io.h"
#include <stdio.h>
#include "readln.h"
#include "pico/sync.h"

uint8_t cd_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    if (sh_argc < 2) {
        strcpy(path_tmp, "/");
        goto cd_done;
    }
    if (strcmp(sh_argv[1], ".") == 0) {
        goto cd_done;
    }
    if (strcmp(sh_argv[1], "..") == 0) {
        if (strcmp(sh_pwd, "/") == 0) {
            strcpy(sh_message, "not a directory");
            return 2;
        }
        strcpy(path_tmp, sh_pwd);
        char* cp = strrchr(path_tmp, '/');
        if (cp == NULL) {
            cp = sh_pwd;
        }
        *cp = 0;
        cp = strrchr(path_tmp, '/');
        if (cp != NULL) {
            *(cp + 1) = 0;
        }
        goto cd_done;
    }
    full_path(sh_argv[1]);
    lfs_dir_t dir;
    if (fs_dir_open(&dir, path_tmp) < LFS_ERR_OK) {
        strcpy(sh_message, "not a directory");
        return 3;
    }
    fs_dir_close(&dir);
cd_done:
    strcpy(sh_pwd, path_tmp);
    if (sh_pwd[strlen(sh_pwd) - 1] != '/') {
        strcat(sh_pwd, "/");
    }
    sprintf(sh_message, "changed to %s", sh_pwd);
    return 0;
}

uint8_t quit_cmd(void) {
    if (!prompt_Yn("are you sure?")) {
        return 1;
    }
    // release any resources we were using
    if (mounted) {
        savehist();
        fs_unmount();
    }
    printf("\nbye!\n");
    sleep_ms(1000);
    exit(0);
    return 0;
}

uint8_t version_cmd(void) {
    printf("\nPico Shell " PSHELL_GIT_TAG ", LittleFS v%d.%d, Vi " VI_VER ", SDK v%d.%d.%d\n",
           LFS_VERSION >> 16, LFS_VERSION & 0xffff, PICO_SDK_VERSION_MAJOR, PICO_SDK_VERSION_MINOR,
           PICO_SDK_VERSION_REVISION);
#if !defined(NDEBUG)
    printf("gcc %s\n", __VERSION__);
#endif
    return 0;
}

uint8_t help_cmd(void) {
    printf("\n");
    for (int i = 0; cmd_table[i].name; i++) {
        printf("%7s - %s\n", cmd_table[i].name, cmd_table[i].descr);
    }
    return 0;
}
