/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Copyright (c) 1883 Thomas Edison - All Rights Reserved
 * You may use, distribute and modify this code under the
 * terms of the BSD 3 clause license, which unfortunately
 * won't be written for another century.
 */

#include "main.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include "hardware/structs/scb.h"
#include "hardware/watchdog.h"

#include "pico/bootrom.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "pico/sync.h"

#include "cc.h"
#include "io.h"
#include "readln.h"
#include "vi.h"
#include "xmodem.h"
#include "ymodem.h"
#if !defined(NDEBUG) || defined(PSHELL_TESTS)
#include "tests.h"
#endif

#include "file_cmds.h"
#include "tar_cmd.h"

// #define COPYRIGHT "\u00a9" // for UTF8
#define COPYRIGHT "(c)" // for ASCII

#define VT_ESC "\033"
#define VT_CLEAR VT_ESC "[H" VT_ESC "[J"
#define VT_BLINK VT_ESC "[5m"
#define VT_BOLD VT_ESC "[1m"
#define VT_NORMAL VT_ESC "[m"

#define MAX_ARGS 16
int sh_argc;
char* sh_argv[MAX_ARGS + 1];

static uint32_t screen_x = 80, screen_y = 24;
static lfs_file_t file;
static buf_t cmd_buffer, path, curdir = "/";
buf_t sh_message;
static bool mounted = false;
static bool run = true;

static void set_translate_crlf(bool enable) {
    stdio_driver_t* driver;
#if LIB_PICO_STDIO_UART
    driver = &stdio_uart;
#endif
#if LIB_PICO_STDIO_USB
    driver = &stdio_usb;
#endif
    stdio_set_translate_crlf(driver, enable);
}

// used by Vi
void get_screen_xy(uint32_t* x, uint32_t* y) {
    *x = screen_x;
    *y = screen_y;
}

static void echo_key(char c) {
    putchar(c);
    if (c == '\r') {
        putchar('\n');
    }
}

char* full_path(const char* name) {
    if (name == NULL) {
        return NULL;
    }
    if (name[0] == '/') {
        strcpy(path, name);
        return path;
    }
    if (strncmp(name, "./", 2) == 0) {
        name += 2;
    }
    strcpy(path, curdir);
    if (strncmp(name, "../", 3) != 0) {
        if (name[0]) {
            strcat(path, name);
        }
    } else {
        name += 3; // root doen't have a parent
        char* cp = strrchr(path, '/');
        if (cp != NULL) {
            *cp = 0;
        }
        cp = strrchr(path, '/');
        if (cp != NULL) {
            *(cp + 1) = 0;
        }
        strcat(path, name);
    }
    return path;
}

static void parse_cmd(void) {
    // read line into buffer
    char* cp = cmd_buffer;
    char* cp_end = cp + sizeof(cmd_buffer);
    char prompt[128];
    snprintf(prompt, sizeof(prompt), VT_BOLD "%s: " VT_NORMAL, full_path(""));
    cp = dgreadln(cmd_buffer, mounted, prompt);
    bool not_last = true;
    for (sh_argc = 0; not_last && (sh_argc < MAX_ARGS); sh_argc++) {
        while (*cp == ' ') {
            cp++; // skip blanks
        }
        if ((*cp == '\r') || (*cp == '\n')) {
            break;
        }
        sh_argv[sh_argc] = cp; // start of string
        while ((*cp != ' ') && (*cp != '\r') && (*cp != '\n')) {
            cp++; // skip non blank
        }
        if ((*cp == '\r') || (*cp == '\n')) {
            not_last = false;
        }
        *cp++ = 0; // terminate string
    }
    sh_argv[sh_argc] = NULL;
}

static int xmodem_rx_cb(uint8_t* buf, uint32_t len) {
    if (fs_file_write(&file, buf, len) != len) {
        printf("error writing file\n");
    }
}

static int xmodem_tx_cb(uint8_t* buf, uint32_t len) { return fs_file_read(&file, buf, len); }

bool bad_mount(bool need) {
    if (mounted == need) {
        return false;
    }
    sprintf(sh_message, "filesystem is %smounted", (need ? "not " : ""));
    return true;
}

bool bad_name(void) {
    if (sh_argc > 1) {
        return false;
    }
    strcpy(sh_message, "missing file or directory name");
    return true;
}

static uint8_t xput_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    if (bad_name()) {
        return 2;
    }
    if (fs_file_open(&file, full_path(sh_argv[1]), LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) <
        LFS_ERR_OK) {
        strcpy(sh_message, "Can't create file");
        return 3;
    }
    set_translate_crlf(false);
    xmodemReceive(xmodem_rx_cb);
    set_translate_crlf(true);
    busy_wait_ms(3000);
    sprintf(sh_message, "\nfile transfered, size: %d", fs_file_seek(&file, 0, LFS_SEEK_END));
    fs_file_close(&file);
    return 0;
}

static uint8_t yput_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    if (sh_argc > 1) {
        strcpy(sh_message, "yput doesn't take a parameter");
        return 2;
    }
    char* tmpname = strdup(full_path("ymodem.tmp"));
    if (fs_file_open(&file, tmpname, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) < LFS_ERR_OK) {
        strcpy(sh_message, "Can't create file");
        return 3;
    }
    set_translate_crlf(false);
    char name[256];
    int res = Ymodem_Receive(&file, 0x7fffffff, name);
    set_translate_crlf(true);
    fs_file_close(&file);
    if (res >= 0) {
        sprintf(sh_message, "\nfile transfered, size: %d", fs_file_seek(&file, 0, LFS_SEEK_END));
        fs_rename(tmpname, full_path(name));
    } else {
        strcpy(sh_message, "File transfer failed");
        fs_remove(tmpname);
    }
    free(tmpname);
    return 0;
}

static uint8_t cat_cmd(void) {
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

static uint8_t xget_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    if (bad_name()) {
        return 2;
    }
    if (fs_file_open(&file, full_path(sh_argv[1]), LFS_O_RDONLY) < LFS_ERR_OK) {
        strcpy(sh_message, "Can't open file");
        return 3;
    }
    set_translate_crlf(false);
    int byte_count = xmodemTransmit(xmodem_tx_cb);
    set_translate_crlf(true);
    fs_file_close(&file);
    return (byte_count < 0) ? 1 : 0;
}

static uint8_t yget_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    if (bad_name()) {
        return 2;
    }
    if (fs_file_open(&file, full_path(sh_argv[1]), LFS_O_RDONLY) < LFS_ERR_OK) {
        strcpy(sh_message, "Can't open file");
        return 3;
    }
    int siz = fs_file_seek(&file, 0, LFS_SEEK_END);
    fs_file_seek(&file, 0, LFS_SEEK_SET);
    set_translate_crlf(false);
    int res = Ymodem_Transmit(full_path(sh_argv[1]), siz, &file);
    set_translate_crlf(true);
    fs_file_close(&file);
    if (res) {
        strcpy(sh_message, "File transfer failed");
        return 4;
    } else {
        sprintf(sh_message, "%d bytes sent", siz);
        return 0;
    }
}

static uint8_t mount_cmd(void) {
    if (bad_mount(false)) {
        return 1;
    }
    if (fs_mount() != LFS_ERR_OK) {
        strcpy(sh_message, "Error mounting filesystem");
        return 2;
    }
    mounted = true;
    strcpy(sh_message, "mounted");
    return 0;
}

static uint8_t unmount_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    if (fs_unmount() != LFS_ERR_OK) {
        strcpy(sh_message, "Error unmounting filesystem");
        return 2;
    }
    mounted = false;
    strcpy(sh_message, "unmounted");
    return 0;
}

static uint8_t format_cmd(void) {
    if (bad_mount(false)) {
        return 1;
    }
    printf("are you sure (y/N) ? ");
    fflush(stdout);
    parse_cmd();
    if ((sh_argc == 0) || ((sh_argv[0][0] | ' ') != 'y')) {
        strcpy(sh_message, "user cancelled");
        return 2;
    }
    if (fs_format() != LFS_ERR_OK) {
        strcpy(sh_message, "Error formating filesystem");
        return 3;
    }
    strcpy(sh_message, "formatted");
    return 0;
}

static void disk_space(uint64_t n, char* buf) {
    double d = n;
    static const char* suffix[] = {"B", "KB", "MB", "GB", "TB"};
    char** sfx = (char**)suffix;
    while (d >= 1000.0) {
        d /= 1000.0;
        sfx++;
    }
    sprintf(buf, "%.1f%s", d, *sfx);
}

static uint8_t status_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    struct fs_fsstat_t stat;
    fs_fsstat(&stat);
    const char percent = 37;
    char total_size[32], used_size[32];
    disk_space((int64_t)stat.block_count * stat.block_size, total_size);
    disk_space((int64_t)stat.blocks_used * stat.block_size, used_size);
#ifndef NDEBUG
    printf("\ntext size 0x%x (%d), bss size 0x%x (%d)", stat.text_size, stat.text_size,
           stat.bss_size, stat.bss_size);
#endif
    sprintf(sh_message,
            "\ntotal blocks %d, block size %d, used %s out of %s, %1.1f%c "
            "used.\n",
            (int)stat.block_count, (int)stat.block_size, used_size, total_size,
            stat.blocks_used * 100.0 / stat.block_count, percent);
    return 0;
}

static uint8_t ls_cmd(void) {
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
                    printf(" %7d [%s]\n", info.size, info.name);
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

static uint8_t cd_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    if (sh_argc < 2) {
        strcpy(path, "/");
        goto cd_done;
    }
    if (strcmp(sh_argv[1], ".") == 0) {
        goto cd_done;
    }
    if (strcmp(sh_argv[1], "..") == 0) {
        if (strcmp(curdir, "/") == 0) {
            strcpy(sh_message, "not a directory");
            return 2;
        }
        strcpy(path, curdir);
        char* cp = strrchr(path, '/');
        if (cp == NULL) {
            cp = curdir;
        }
        *cp = 0;
        cp = strrchr(path, '/');
        if (cp != NULL) {
            *(cp + 1) = 0;
        }
        goto cd_done;
    }
    full_path(sh_argv[1]);
    lfs_dir_t dir;
    if (fs_dir_open(&dir, path) < LFS_ERR_OK) {
        strcpy(sh_message, "not a directory");
        return 3;
    }
    fs_dir_close(&dir);
cd_done:
    strcpy(curdir, path);
    if (curdir[strlen(curdir) - 1] != '/') {
        strcat(curdir, "/");
    }
    sprintf(sh_message, "changed to %s", curdir);
    return 0;
}

static uint8_t cc_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    if (!cc(0, sh_argc, sh_argv)) {
        return 1;
    } else {
        return 0;
    }
}

#if !defined(NDEBUG) || defined(PSHELL_TESTS)
static uint8_t tests_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    // TODO: make run_tests return an exit status.
    run_tests(sh_argc, sh_argv);
    return 0;
}
#endif

static uint8_t vi_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    // TODO: vi always returns 0
    vi(sh_argc - 1, sh_argv + 1);
    return 0;
}

static uint8_t clear_cmd(void) {
    strcpy(sh_message, VT_CLEAR "\n");
    return 0;
}

static uint8_t reboot_cmd(void) {
    // release any resources we were using
    if (mounted) {
        savehist();
        fs_unmount();
    }
    watchdog_reboot(0, 0, 1);
    return 0;
}

#if LIB_PICO_STDIO_USB
static uint8_t usbboot_cmd(void) {
    // release any resources we were using
    if (mounted) {
        savehist();
        fs_unmount();
    }
    reset_usb_boot(0, 0);
    return 0;
}
#endif

static uint8_t quit_cmd(void) {
    printf("\nare you sure (Y/n) ? ");
    fflush(stdout);
    char c = getchar();
    putchar('\n');
    if (c != 'y' && c != 'Y' && c != '\r') {
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

static uint8_t version_cmd(void) {
    printf("\nPico Shell " PSHELL_GIT_TAG ", LittleFS v%d.%d, Vi " VI_VER ", SDK v%d.%d.%d\n",
           LFS_VERSION >> 16, LFS_VERSION & 0xffff, PICO_SDK_VERSION_MAJOR, PICO_SDK_VERSION_MINOR,
           PICO_SDK_VERSION_REVISION);
#if !defined(NDEBUG)
    printf("gcc %s\n", __VERSION__);
#endif
    return 0;
}

static bool cursor_pos(uint32_t* x, uint32_t* y) {
    int rc = false;
    *x = 80;
    *y = 24;
    do {
        printf(VT_ESC "[6n");
        fflush(stdout);
        int k = getchar_timeout_us(100000);
        if (k == PICO_ERROR_TIMEOUT) {
            break;
        }
        char* cp = cmd_buffer;
        while (cp < cmd_buffer + sizeof cmd_buffer) {
            k = getchar_timeout_us(100000);
            if (k == PICO_ERROR_TIMEOUT) {
                break;
            }
            *cp++ = k;
        }
        if (cp == cmd_buffer) {
            break;
        }
        if (cmd_buffer[0] != '[') {
            break;
        }
        *cp = 0;
        if (cp - cmd_buffer < 5) {
            break;
        }
        char* end;
        uint32_t row, col;
        if (!isdigit(cmd_buffer[1])) {
            break;
        }
        errno = 0;
        row = strtoul(cmd_buffer + 1, &end, 10);
        if (errno) {
            break;
        }
        if (*end != ';' || !isdigit(end[1])) {
            break;
        }
        col = strtoul(end + 1, &end, 10);
        if (errno) {
            break;
        }
        if (*end != 'R') {
            break;
        }
        if (row < 1 || col < 1 || (row | col) > 0x7fff) {
            break;
        }
        *x = col;
        *y = row;
        rc = true;
    } while (false);
    return rc;
}

static bool screen_size(void) {
    int rc = false;
    screen_x = 80;
    screen_y = 24;
    uint32_t cur_x, cur_y;
    do {
        set_translate_crlf(false);
        if (!cursor_pos(&cur_x, &cur_y)) {
            break;
        }
        printf(VT_ESC "[999;999H");
        if (!cursor_pos(&screen_x, &screen_y)) {
            break;
        }
        if (cur_x > screen_x) {
            cur_x = screen_x;
        }
        if (cur_y > screen_y) {
            cur_y = screen_y;
        }
        printf("\033[%d;%dH", cur_y, cur_x);
        fflush(stdout);
        rc = true;
    } while (false);
    set_translate_crlf(true);
    return rc;
}

static uint8_t resize_cmd(void) {
    if (!screen_size()) {
        return 1;
    } else {
        return 0;
    }
}

// clang-format off
cmd_t cmd_table[] = {
    {"cat",     cat_cmd,        "display a text file"},
    {"cc",      cc_cmd,         "compile & run C source file. cc -h for help"},
    {"cd",      cd_cmd,         "change directory"},
    {"clear",   clear_cmd,      "clear the screen"},
    {"cp",      cp_cmd,         "copy a file"},
    {"format",  format_cmd,     "format the filesystem"},
    {"ls",      ls_cmd,         "list a directory, -a to show hidden files"},
    {"mkdir",   mkdir_cmd,      "create a directory"},
    {"mount",   mount_cmd,      "mount the filesystem"},
    {"mv",      mv_cmd,         "rename a file or directory"},
    {"quit",    quit_cmd,       "shutdown the system"},
    {"reboot",  reboot_cmd,     "restart the system"},
    {"resize",  resize_cmd,     "establish screen dimensions"},
    {"rm",      rm_cmd,         "remove a file or directory. -r for recursive"},
    {"status",  status_cmd,     "display the filesystem status"},
    {"tar",     tar_cmd,        "manage tar archives"},
#if !defined(NDEBUG) || defined(PSHELL_TESTS)
    {"tests",   tests_cmd,      "run all tests"},
#endif
    {"unmount", unmount_cmd,    "unmount the filesystem"},
	{"version", version_cmd,    "display pico shell's version"},
    {"vi",      vi_cmd,         "edit file(s) with vi"},
    {"xget",    xget_cmd,       "get a file (xmodem)"},
    {"xput",    xput_cmd,       "put a file (xmodem)"},
    {"yget",    yget_cmd,       "get a file (ymodem)"},
    {"yput",    yput_cmd,       "put a file (ymodem)"},
	{0}
};
// clang-format on

static uint8_t help(void) {
    printf("\n");
    for (int i = 0; cmd_table[i].name; i++) {
        printf("%7s - %s\n", cmd_table[i].name, cmd_table[i].descr);
    }
    return 0;
}

static const char* search_cmds(int len) {
    if (len == 0) {
        return NULL;
    }
    int i, last_i, count = 0;
    for (i = 0; cmd_table[i].name; i++) {
        if (strncmp(cmd_buffer, cmd_table[i].name, len) == 0) {
            last_i = i;
            count++;
        }
    }
    if (count != 1) {
        return NULL;
    }
    return cmd_table[last_i].name + len;
}

static void HardFault_Handler(void) {
    static const char* clear = "\n\n" VT_BOLD "*** " VT_BLINK "CRASH" VT_NORMAL VT_BOLD
                               " - Rebooting in 5 seconds ***" VT_NORMAL "\r\n\n";
    for (const char* cp = clear; *cp; cp++) {
        putchar(*cp);
    }
#ifndef NDEBUG
    for (;;)
        ;
#endif
    watchdog_reboot(0, 0, 5000);
    for (;;) {
        __wfi();
    }
}

static bool run_as_cmd(const char* dir) {
    char* tfn;
    if (strlen(dir) == 0) {
        tfn = full_path(sh_argv[0]);
    } else {
        if (sh_argv[0][0] == '/') {
            return false;
        }
        tfn = sh_argv[0];
    }
    char* fn = malloc(strlen(tfn) + 6);
    strcpy(fn, dir);
    strcat(fn, tfn);
    char buf[4];
    if (fs_getattr(fn, 1, buf, sizeof(buf)) != sizeof(buf)) {
        free(fn);
        return false;
    }
    if (strcmp(buf, "exe")) {
        free(fn);
        return false;
    }
    sh_argv[0] = fn;
    cc(1, sh_argc, sh_argv);
    free(fn);
    return true;
}

// Print the shell prompt, including any non-zero exit status of the previous command.
static void print_prompt(uint8_t previous_exit_status) {
        printf("\n" VT_BOLD "%s$ ", full_path(""));
        if (previous_exit_status != 0) {
            printf("[%i] ", previous_exit_status);
        }
        printf(VT_NORMAL);
}

// application entry point
int main(void) {
    // initialize the pico SDK
    stdio_init_all();
    bool uart = true;
#if LIB_PICO_STDIO_USB
    while (!stdio_usb_connected()) {
        sleep_ms(1000);
    }
    uart = false;
#endif
    ((int*)scb_hw->vtor)[3] = (int)HardFault_Handler;
    getchar_timeout_us(1000);
    bool detected = screen_size();
    printf(VT_CLEAR);
    fflush(stdout);
    printf("\n" VT_BOLD "Pico Shell" VT_NORMAL " - Copyright " COPYRIGHT " 1883 Thomas Edison\n"
           "This program comes with ABSOLUTELY NO WARRANTY.\n"
           "This is free software, and you are welcome to redistribute it\n"
           "under certain conditions. See LICENSE.md file for details.\n");
    char buf[16];
#if defined(VGABOARD_SD_CLK_PIN)
    strcpy(buf, "sd card");
#else
    strcpy(buf, "internal flash");
#endif
    version_cmd();
    char console[8];
    if (uart) {
#if defined(PICO_DEFAULT_UART)
        sprintf(console, "UART%d", PICO_DEFAULT_UART);
#else
        strcpy(console, "UART");
#endif
    } else {
        strcpy(console, "USB");
    }
    printf("\nboard: " PICO_BOARD ", console: %s [%u X %u], filesystem: %s\n\n"
           "enter command or hit ENTER for help\n\n",
           console, screen_x, screen_y, buf);
    if (!detected) {
        printf("\nYour terminal does not respond to standard VT100 escape sequences"
               "\nsequences. The editor will likely not work at all!");
        fflush(stdout);
    }

    if (fs_load() != LFS_ERR_OK) {
        printf("Can't access filesystem device! Aborting.\n");
        exit(-1);
    }
    if (fs_mount() != LFS_ERR_OK) {
        printf("The flash file system appears corrupt or unformatted!\n"
               " would you like to format it (Y/n) ? ");
        fflush(stdout);
        char c = getchar();
        while (c != 'y' && c != 'Y' && c != 'N' && c != 'n' && c != '\r') {
            c = getchar();
        }
        putchar(c);
        if (c != '\r') {
            echo_key('\r');
        }
        putchar('\n');
        if (c == 'y' || c == 'y') {
            if (fs_format() != LFS_ERR_OK) {
                printf("Error formating file system!\n");
            } else {
                if (fs_mount() != LFS_ERR_OK) {
                    printf("Error formating file system!\n");
                } else {
                    printf("file system formatted and mounted\n");
                    mounted = true;
                }
            }
        }
    } else {
        printf("file system automatically mounted\n");
        mounted = true;
    }
    uint8_t last_ret = 0;
    while (run) {
        print_prompt(last_ret);
        fflush(stdout);
        parse_cmd();
        sh_message[0] = 0;
        bool found = false;
        if (sh_argc) {
            if (!strcmp(sh_argv[0], "q")) {
                quit_cmd();
                continue;
            }
            for (int i = 0; cmd_table[i].name; i++) {
                if (strcmp(sh_argv[0], cmd_table[i].name) == 0) {
                    last_ret = cmd_table[i].func();
                    if (sh_message[0]) {
                        printf("\n%s\n", sh_message);
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (!run_as_cmd("") && !run_as_cmd("/bin/")) {
                    printf("\nunknown command '%s'. hit ENTER for help\n", sh_argv[0]);
                }
            }
        } else {
            last_ret = help();
        }
    }
    fs_unload();
    printf("\ndone\n");
    sleep_ms(1000);
}