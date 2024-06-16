#include "tar_cmd.h"
#include "tar.h"
#include "main.h"

uint8_t tar_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    // TODO: make tar return an exit status.
    tar(sh_argc, sh_argv);
    return 0;
}
