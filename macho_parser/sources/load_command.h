#ifndef LOAD_COMMAND_H
#define LOAD_COMMAND_H

#include <stdbool.h>
#include <mach-o/loader.h>
#include <vector>

std::vector<struct load_command *> parseLoadCommands(uint8_t *Base, int offset, uint32_t ncmds);

typedef struct load_command_with_index {
    int index;
    struct load_command *lcmd;
} load_command_with_index;

// Search a load command by criteria
load_command_with_index search_load_command(uint8_t *base, int start_index, bool (*criteria)(struct load_command *lcmd));

#endif /* LOAD_COMMAND_H */
