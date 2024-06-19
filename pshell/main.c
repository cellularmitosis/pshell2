/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Copyright (c) 1883 Thomas Edison - All Rights Reserved
 * You may use, distribute and modify this code under the
 * terms of the BSD 3 clause license, which unfortunately
 * won't be written for another century.
 */

#include "main.h"

#include <stdio.h>

#include "hardware/structs/scb.h"
#include "hardware/watchdog.h"

#include "pico/stdlib.h"
#include "pico/sync.h"
#include "pico/malloc.h"

#include "cc.h"
#include "io.h"
#include "readln.h"
#if !defined(NDEBUG) || defined(PSHELL_TESTS)
#include "tests.h"
#endif

#include "cc_cmds.h"
#include "cpu_cmds.h"
#include "file_cmds.h"
#include "fs_cmds.h"
#include "modem_cmds.h"
#include "shell_cmds.h"
#include "tar_cmd.h"
#include "terminal_cmds.h"
#include "vi_cmd.h"

#include "terminal.h"

// Shell global state:
#define MAX_ARGS 16
int sh_argc;
char* sh_argv[MAX_ARGS + 1];
buf_t sh_pwd = "/";

// Terminal global state.
uint32_t term_cols = 80;
uint32_t term_rows = 24;

// Filesystem global state.
bool mounted = false;

// Temporaries,  shared state between functions.
buf_t sh_cmd_buffer;
buf_t path_tmp;
buf_t sh_message;

static bool run = true;

// Are we using the UART or USB for the terminal connection?
static bool uart = true;

// clang-format off
cmd_t cmd_table[] = {
    {"cat",     cat_cmd,        "display a text file"},
    {"cc",      cc_cmd,         "compile & run C source file. cc -h for help"},
    {"cd",      cd_cmd,         "change directory"},
    {"clear",   clear_cmd,      "clear the screen"},
    {"cp",      cp_cmd,         "copy a file"},
    {"df",      df_cmd,         "display the filesystem usage"},
    {"format",  format_cmd,     "format the filesystem"},
    {"help",    help_cmd,       "display this message"},
    {"ls",      ls_cmd,         "list a directory, -a to show hidden files"},
    {"mkdir",   mkdir_cmd,      "create a directory"},
    {"mount",   mount_cmd,      "mount the filesystem"},
    {"mv",      mv_cmd,         "rename a file or directory"},
    {"quit",    quit_cmd,       "shutdown the system"},
    {"reboot",  reboot_cmd,     "restart the system"},
    {"resize",  resize_cmd,     "establish screen dimensions"},
    {"rm",      rm_cmd,         "remove a file or directory. -r for recursive"},
    {"tar",     tar_cmd,        "manage tar archives"},
#if !defined(NDEBUG) || defined(PSHELL_TESTS)
    {"tests",   tests_cmd,      "run all tests"},
#endif
    {"unmount", unmount_cmd,    "unmount the filesystem"},
#if LIB_PICO_STDIO_USB
    {"usbboot", usbboot_cmd,    "reboot into the USB bootloader"},
#endif
	{"version", version_cmd,    "display pico shell's version"},
    {"vi",      vi_cmd,         "edit file(s) with vi"},
    {"xget",    xget_cmd,       "get a file (xmodem)"},
    {"xput",    xput_cmd,       "put a file (xmodem)"},
    {"yget",    yget_cmd,       "get a file (ymodem)"},
    {"yput",    yput_cmd,       "put a file (ymodem)"},
	{0}
};
// clang-format on

void set_translate_crlf(bool enable) {
    stdio_driver_t* driver;
#if LIB_PICO_STDIO_UART
    driver = &stdio_uart;
#endif
#if LIB_PICO_STDIO_USB
    driver = &stdio_usb;
#endif
    stdio_set_translate_crlf(driver, enable);
}

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

static void echo_key(char c) {
    putchar(c);
    if (c == '\r') {
        putchar('\n');
    }
}

static bool prompt_yn(char* prompt, char default_) {
    while(true) {
        printf(prompt);
        printf(" [Y/n] ");
        fflush(stdout);
        char c = getchar();
        if (c == '\r') {
            c = default_;
        }
        if (c == 'y' || c == 'Y') {
            return true;
        }
        if (c == 'n' || c == 'N') {
            return false;
        }
        printf("'%c' is not a valid response.\n", c);
    }
}

bool prompt_Yn(char* prompt) {
    return prompt_yn(prompt, 'y');
}

bool prompt_yN(char* prompt) {
    return prompt_yn(prompt, 'n');
}

char* full_path(const char* name) {
    if (name == NULL) {
        return NULL;
    }
    if (name[0] == '/') {
        strcpy(path_tmp, name);
        return path_tmp;
    }
    if (strncmp(name, "./", 2) == 0) {
        name += 2;
    }
    strcpy(path_tmp, sh_pwd);
    if (strncmp(name, "../", 3) != 0) {
        if (name[0]) {
            strcat(path_tmp, name);
        }
    } else {
        name += 3; // root doen't have a parent
        char* cp = strrchr(path_tmp, '/');
        if (cp != NULL) {
            *cp = 0;
        }
        cp = strrchr(path_tmp, '/');
        if (cp != NULL) {
            *(cp + 1) = 0;
        }
        strcat(path_tmp, name);
    }
    return path_tmp;
}

static void parse_sh_command(void) {
    // read line into buffer
    char* cp = sh_cmd_buffer;
    char* cp_end = cp + sizeof(sh_cmd_buffer);
    char prompt[128];
    snprintf(prompt, sizeof(prompt), VT_BOLD "%s: " VT_NORMAL, full_path(""));
    cp = dgreadln(sh_cmd_buffer, mounted, prompt);
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

static const char* search_cmds(int len) {
    if (len == 0) {
        return NULL;
    }
    int i, last_i, count = 0;
    for (i = 0; cmd_table[i].name; i++) {
        if (strncmp(sh_cmd_buffer, cmd_table[i].name, len) == 0) {
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
static void print_sh_prompt(uint8_t previous_exit_status) {
    printf("\n" VT_BOLD "%s$ ", full_path(""));
    if (previous_exit_status != 0) {
        printf("[%i] ", previous_exit_status);
    }
    printf(VT_NORMAL);
}

void init() {
    // initialize the pico SDK
    stdio_init_all();

    uart = true;
#if LIB_PICO_STDIO_USB
    while (!stdio_usb_connected()) {
        sleep_ms(1000);
    }
    uart = false;
#endif

    ((int*)scb_hw->vtor)[3] = (int)HardFault_Handler;

    getchar_timeout_us(1000);

    fflush(stdout);
    printf(VT_CLEAR);
}

void print_startup_banner() {
    bool detected = screen_size();
    printf("\n" VT_BOLD "Pico Shell" VT_NORMAL " - Copyright (c) 1883 Thomas Edison\n"
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
           console, term_rows, term_cols, buf);
    if (!detected) {
        printf("\nYour terminal does not respond to standard VT100 escape sequences"
               "\nsequences. The editor will likely not work at all!");
        fflush(stdout);
    }
}

void disk_init() {
    if (fs_load() != LFS_ERR_OK) {
        printf("Can't access filesystem device! Aborting.\n");
        exit(-1);
    }
    if (fs_mount() != LFS_ERR_OK) {
        // TODO: refactor this to use prompt_Yn, format_cmd and mount_cmd.
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
}

// print a peak malloc usage after running each command:
#define TRACE_MALLOC 0
// note: this requires some changes to pico/malloc.c:
/*
size_t malloc_allocated_bytes = 0;
size_t malloc_peak_bytes = 0;
size_t malloc_allocation_count = 0;
size_t malloc_peak_allocations = 0;
void *WRAPPER_FUNC(malloc)(size_t size) {
    malloc_allocation_count += 1;
    if (malloc_allocation_count > malloc_peak_allocations) {
        malloc_peak_allocations = malloc_peak_allocations;
    }
    malloc_allocated_bytes += size;
    if (malloc_allocated_bytes > malloc_peak_bytes) {
        malloc_peak_bytes = malloc_allocated_bytes;
    }
*/
// and to pico/malloc.h:
/*
#include <stdlib.h>
extern size_t malloc_allocated_bytes;
extern size_t malloc_peak_bytes;
extern size_t malloc_allocation_count;
extern size_t malloc_peak_allocations;
*/

void sh_repl() {
    uint8_t last_ret = 0;
    while (run) {
        print_sh_prompt(last_ret);
        fflush(stdout);
        parse_sh_command();
        sh_message[0] = 0;
        bool found = false;
        if (sh_argc) {
            if (!strcmp(sh_argv[0], "q")) {
                quit_cmd();
                continue;
            }
            for (int i = 0; cmd_table[i].name; i++) {
                if (strcmp(sh_argv[0], cmd_table[i].name) == 0) {
                    #ifdef TRACE_MALLOC
                    malloc_allocation_count = 0;
                    malloc_allocated_bytes = 0;
                    malloc_peak_allocations = 0;
                    malloc_peak_bytes = 0;
                    #endif
                    last_ret = cmd_table[i].func();
                    if (sh_message[0]) {
                        printf("\n%s\n", sh_message);
                    }
                    #ifdef TRACE_MALLOC
                    printf("\nmalloc: peak bytes: %i, peak allocations: %i\n", malloc_peak_bytes, malloc_peak_allocations);
                    // jun 18 2024: 'cc doughnut.c': peak = 53284 bytes
                    #endif
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
            last_ret = help_cmd();
        }
    }
}

int main(void) {
    init();
    print_startup_banner();
    disk_init();

    sh_repl();

    fs_unload();
    printf("\ndone\n");
    sleep_ms(1000);
}
