#ifndef MACHO_BINARY_H
#define MACHO_BINARY_H

#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <vector>
#include <filesystem>

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

    std::string getDylibNameByOrdinal(int ordinal, bool basename = true) {
        if (ordinal > 0 && ordinal <= MAX_LIBRARY_ORDINAL) { // 0 ~ 253
            struct dylib_command *dylibCmd = getDylibCommands()[ordinal - 1];
            std::filesystem::path dylibPath = std::filesystem::path((char *)dylibCmd + dylibCmd->dylib.name.offset);
            if (basename) {
                dylibPath = dylibPath.filename();
            }
            return dylibPath.string();
        } else if (ordinal == DYNAMIC_LOOKUP_ORDINAL) { // 254
            return "dynamic lookup";
        } else if (ordinal == EXECUTABLE_ORDINAL) { // 255
            return "exectuable";
        }
        return "invalid ordinal";
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

    std::string getSectionNameByOrdinal(int ordinal) {
        int index = 1;
        for (auto segCmd : segmentCommands) {
            struct section_64 *sections =
                (struct section_64 *)((uint8_t *)segCmd + sizeof(struct segment_command_64));

            for (int i = 0; i < segCmd->nsects; ++i) {
                struct section_64 *sect = sections + i;
                if (index == ordinal) {
                    std::string segname(sect->segname);
                    std::string sectname(sect->sectname);
                    std::string fullName = std::string("(") + segname.c_str() + ", " + sectname.c_str() + ")";
                    return fullName;
                }
                index++;
            }
        }

        return nullptr;
    }

private:
    std::vector<struct dylib_command *> dylibCommands;
};

extern struct MachoBinary machoBinary;

#endif /* MACHO_BINARY_H */
