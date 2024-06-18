#include "cc_cmds.h"
#include "main.h"
#include "cc.h"

uint8_t cc_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    int ret = cc(0, sh_argc, sh_argv);
    if (ret != 0) {
        return 1;
    } else {
        return 0;
    }
}
