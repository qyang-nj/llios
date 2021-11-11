#ifndef LOAD_COMMAND_H
#define LOAD_COMMAND_H

#include <stdbool.h>

typedef struct load_command_with_index {
    int index;
    struct load_command *lcmd;
} load_command_with_index;

// Search a load command by criteria
load_command_with_index search_load_command(void *base, int start_index, bool (*criteria)(struct load_command *lcmd));

#endif /* LOAD_COMMAND_H */
