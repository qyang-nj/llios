#include <stdio.h>

#include "macho_header.h"
#include "load_command.h"

load_command_with_index search_load_command(void *base, int start_index, bool (*criteria)(struct load_command *lcmd)) {
    struct mach_header_64 *mach_header = parse_mach_header(base);

    int offset = sizeof(struct mach_header_64);
    for (int i = start_index; i < mach_header->ncmds; ++i) {
        struct load_command *lcmd = base + offset;

        if ((*criteria)(lcmd)) {
            load_command_with_index result = { i, lcmd };
            return result;
        }

        offset += lcmd->cmdsize;
    }

    // not found
    load_command_with_index result = { -1, NULL };
    return result;
}
