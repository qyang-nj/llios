#ifndef MACHO_BINARY_H
#define MACHO_BINARY_H

#include <vector>

struct MachoBinary {
    uint8_t *base;
    std::vector<struct load_command *> allLoadCommands;
    std::vector<struct segment_command_64 *> segmentCommands;
};

extern struct MachoBinary machoBinary;

#endif /* MACHO_BINARY_H */
