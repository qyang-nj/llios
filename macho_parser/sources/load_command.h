#ifndef LOAD_COMMAND_H
#define LOAD_COMMAND_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct load_command_with_index {
    int index;
    struct load_command *lcmd;
} load_command_with_index;

// Search a load command by criteria
load_command_with_index search_load_command(uint8_t *base, int start_index, bool (*criteria)(struct load_command *lcmd));

#ifdef __cplusplus
}
#endif

#endif /* LOAD_COMMAND_H */
