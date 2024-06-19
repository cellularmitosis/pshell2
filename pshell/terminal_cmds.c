#include "terminal_cmds.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "main.h"
#include "terminal.h"
#include <errno.h>
#include <ctype.h>
#include "pico/error.h"
#include "pico/stdio.h"

uint8_t clear_cmd(void) {
    strcpy(sh_message, VT_CLEAR "\n");
    return 0;
}

bool cursor_pos(uint32_t* x, uint32_t* y) {
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
        char* cp = sh_cmd_buffer;
        while (cp < sh_cmd_buffer + sizeof sh_cmd_buffer) {
            k = getchar_timeout_us(100000);
            if (k == PICO_ERROR_TIMEOUT) {
                break;
            }
            *cp++ = k;
        }
        if (cp == sh_cmd_buffer) {
            break;
        }
        if (sh_cmd_buffer[0] != '[') {
            break;
        }
        *cp = 0;
        if (cp - sh_cmd_buffer < 5) {
            break;
        }
        char* end;
        uint32_t row, col;
        if (!isdigit(sh_cmd_buffer[1])) {
            break;
        }
        errno = 0;
        row = strtoul(sh_cmd_buffer + 1, &end, 10);
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

bool screen_size(void) {
    int rc = false;
    term_cols = 80;
    term_rows = 24;
    uint32_t cur_x, cur_y;
    do {
        set_translate_crlf(false);
        if (!cursor_pos(&cur_x, &cur_y)) {
            break;
        }
        printf(VT_ESC "[999;999H");
        if (!cursor_pos(&term_cols, &term_rows)) {
            break;
        }
        if (cur_x > term_cols) {
            cur_x = term_cols;
        }
        if (cur_y > term_rows) {
            cur_y = term_rows;
        }
        printf("\033[%d;%dH", cur_y, cur_x);
        fflush(stdout);
        rc = true;
    } while (false);
    set_translate_crlf(true);
    return rc;
}

uint8_t resize_cmd(void) {
    if (!screen_size()) {
        return 1;
    } else {
        return 0;
    }
}
