#include "terminal_cmds.h"
#include "vi.h"
#include "main.h"

uint8_t vi_cmd(void) {
    if (bad_mount(true)) {
        return 1;
    }
    // TODO: vi always returns 0
    vi(sh_argc - 1, sh_argv + 1);
    return 0;
}
