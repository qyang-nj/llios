#include <stdio.h>

#include "macho_binary.h"
#include "macho_header.h"

#include "load_command.h"

std::vector<struct load_command *> parseLoadCommands(uint8_t *Base, int offset, uint32_t ncmds) {
    std::vector<struct load_command *> allLoadCommands;
    for (int i = 0; i < ncmds; ++i) {
        struct load_command *lcmd = (struct load_command *)(Base + offset);
        allLoadCommands.push_back(lcmd);
        offset += lcmd->cmdsize;
    }
    return allLoadCommands;
}

load_command_with_index search_load_command(uint8_t *base, int start_index, bool (*criteria)(struct load_command *lcmd)) {
    auto allLoadCommands = machoBinary.allLoadCommands;
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
