#include "fs_cmds.h"
#include <stdio.h>
#include "io.h"
#include "main.h"

uint8_t format_cmd(void) {
    if (bad_mount(false)) {
        return 1;
    }
    if (!prompt_yN("are you sure?")) {
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

uint8_t mount_cmd(void) {
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

uint8_t unmount_cmd(void) {
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

uint8_t status_cmd(void) {
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
