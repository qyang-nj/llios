#include <stdio.h>

#include "macho_binary.h"
#include "macho_header.h"

#include "load_command.h"

load_command_with_index search_load_command(uint8_t *base, int start_index, bool (*criteria)(struct load_command *lcmd)) {
    auto allLoadCommands = machoBinary.all_load_commands;
    for(int i = 0; i < allLoadCommands.size(); ++i) {
        struct load_command *lcmd = allLoadCommands[i];
        if ((*criteria)(lcmd)) {
            load_command_with_index result = { i, lcmd };
            return result;
        }
    }

    // not found
    load_command_with_index result = { -1, NULL };
    return result;
}
