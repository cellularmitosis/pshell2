#include "modem_cmds.h"
#include "xmodem.h"
#include "ymodem.h"
#include "main.h"
#include <stdio.h>
#include "hardware/timer.h"

static lfs_file_t file;

static int xmodem_rx_cb(uint8_t* buf, uint32_t len) {
    if (fs_file_write(&file, buf, len) != len) {
        printf("error writing file\n");
    }
}

static int xmodem_tx_cb(uint8_t* buf, uint32_t len) {
    return fs_file_read(&file, buf, len);
}

uint8_t xget_cmd(void) {
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

uint8_t xput_cmd(void) {
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

uint8_t yget_cmd(void) {
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

uint8_t yput_cmd(void) {
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
