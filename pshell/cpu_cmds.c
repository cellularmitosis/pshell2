#include "cpu_cmds.h"
#include "main.h"
#include "io.h"
#include "readln.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"

uint8_t reboot_cmd(void) {
    // release any resources we were using
    if (mounted) {
        savehist();
        fs_unmount();
    }
    watchdog_reboot(0, 0, 1);
    return 0;
}

#if LIB_PICO_STDIO_USB
uint8_t usbboot_cmd(void) {
    // release any resources we were using
    if (mounted) {
        savehist();
        fs_unmount();
    }
    reset_usb_boot(0, 0);
    return 0;
}
#endif
