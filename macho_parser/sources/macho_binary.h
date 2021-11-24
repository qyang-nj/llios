#ifndef MACHO_BINARY_H
#define MACHO_BINARY_H

#include <mach-o/loader.h>
#include <vector>

struct MachoBinary {
    uint8_t *base;
    std::vector<struct load_command *> allLoadCommands;
    std::vector<struct segment_command_64 *> segmentCommands;

    std::vector<struct dylib_command *> &getDylibCommands() {
        if (this->dylibCommands.size() == 0) {
            std::vector<struct load_command *> dylibCommands;
            std::copy_if(allLoadCommands.begin(), allLoadCommands.end(), std::back_inserter(dylibCommands),
                [](struct load_command * lcmd){
                    return lcmd->cmd == LC_LOAD_DYLIB
                        || lcmd->cmd == LC_LOAD_WEAK_DYLIB
                        || lcmd->cmd == LC_REEXPORT_DYLIB
                        || lcmd->cmd == LC_PREBOUND_DYLIB
                        || lcmd->cmd == LC_LAZY_LOAD_DYLIB
                        || lcmd->cmd == LC_LOAD_UPWARD_DYLIB;
                });
            std::transform(dylibCommands.begin(), dylibCommands.end(), std::back_inserter(this->dylibCommands),
                [](struct load_command * lcmd){ return (struct dylib_command *)lcmd; });
        }
        return this->dylibCommands;
    }

    struct section_64 *getSectionByAddress(uint64_t addr) {
        for (auto segCmd : segmentCommands) {
            struct section_64 *sections =
                (struct section_64 *)((uint8_t *)segCmd + sizeof(struct segment_command_64));

            for (int i = 0; i < segCmd->nsects; ++i) {
                struct section_64 *sect = sections + i;
                if (addr >= sect->addr && addr < (sect->addr + sect->size)) {
                    return sect;
                }
            }
        }

        return nullptr;
    }

private:
    std::vector<struct dylib_command *> dylibCommands;
};

extern struct MachoBinary machoBinary;

#endif /* MACHO_BINARY_H */
