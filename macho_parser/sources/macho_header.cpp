#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <mach-o/fat.h>
#include <mach-o/swap.h>
#include <string>
#include <iomanip>
#include <sstream>
#include <vector>
#include <map>

extern "C" {
#include "util.h"
#include "argument.h"
}

#include "macho_header.h"

#define NEEDS_SWAP(magic) (magic == FAT_CIGAM || magic == FAT_CIGAM_64 || magic == MH_CIGAM || magic == MH_CIGAM_64)

static uint32_t readMagic(uint8_t *base, int offset);
static struct fat_header readFatHeader(uint8_t *base, bool needs_swap);
static struct fat_arch *readFatArchs(uint8_t *base, struct fat_header header, bool needs_swap);
static struct mach_header_64 readMachHeader(uint8_t *base, uint64_t offset);

static void printFatHeader(uint32_t magic, struct fat_header header);
static void printFatArchs(struct fat_arch *archs, int nfat_arch);
static void printMachHeader(struct mach_header_64 header);

static std::string stringifyMagic(uint32_t magic);
static std::string stringifyCPUType(cpu_type_t cputype);
static std::string stringifyCPUSubType(cpu_type_t cputype,  cpu_subtype_t cpusubtype);
static std::string stringifyFileType(uint32_t filetype);
static std::string stringifyHeaderFlags(uint32_t flags);

struct mach_header_64 *parseMachHeader(uint8_t *base) {
    uint32_t magic = readMagic(base, 0);
    int machHeaderOffset = 0;
    const char *cpuType;

    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        struct fat_header header = readFatHeader(base, NEEDS_SWAP(magic));
        struct fat_arch *fat_archs = readFatArchs(base, header, NEEDS_SWAP(magic));

        if (show_header()) {
            printFatHeader(magic, header);
            printFatArchs(fat_archs, header.nfat_arch);
        }

        for (int i = 0; i < header.nfat_arch; ++i) {
            cpuType = stringifyCPUType(fat_archs[i].cputype).c_str();
            if ((fat_archs[i].cputype & CPU_ARCH_ABI64) && is_selected_arch(cpuType)) {
                machHeaderOffset = fat_archs[i].offset;
                break;
            }
        }

        free(fat_archs);

        if (machHeaderOffset == 0) {
            if (args.arch != NULL) {
                fprintf (stderr, "The binary doesn't contain %s architecture.\n", args.arch);
            } else {
                fprintf (stderr, "The binary doesn't contain any 64-bit architecture.\n");
            }
            exit(1);
        }
    }

    magic = readMagic(base, machHeaderOffset);
    if (magic != MH_MAGIC_64) {
        fprintf (stderr, "Magic %s is not recognized or supported.\n", stringifyMagic(magic).c_str());
        exit(1);
    }

    struct mach_header_64 header = readMachHeader(base, machHeaderOffset);
    if (machHeaderOffset == 0) { // non-fat binary
        cpuType = stringifyCPUType(header.cputype).c_str();
        if (!is_selected_arch(cpuType)) {
            fprintf (stderr, "The binary doesn't contain %s architecture.\n", args.arch);
            exit(1);
        }
    }

    if (show_header()) {
        printMachHeader(header);
    }

    return (struct mach_header_64 *)(base + machHeaderOffset);
}

static uint32_t readMagic(uint8_t *filebase, int offset) {
    uint32_t magic = *(uint32_t *)(filebase + offset);
    return magic;
}

static struct fat_header readFatHeader(uint8_t *base, bool needs_swap) {
    struct fat_header header = *(struct fat_header *)base;
    if (needs_swap) {
        swap_fat_header(&header, NX_UnknownByteOrder);
    }
    return header;
}

static struct fat_arch *readFatArchs(uint8_t *base, struct fat_header header, bool needs_swap) {
    size_t archs_size = sizeof(struct fat_arch) * header.nfat_arch;
    struct fat_arch *archs = (struct fat_arch *)malloc(archs_size);
    memcpy(archs, base + sizeof(header), archs_size);

    if (needs_swap) {
        swap_fat_arch(archs, header.nfat_arch, NX_UnknownByteOrder);
    }
    return archs;
}

static struct mach_header_64 readMachHeader(uint8_t *base, uint64_t offset) {
    struct mach_header_64 header = *(struct mach_header_64 *)(base + offset);
    return header;
}

static void printFatHeader(uint32_t magic, struct fat_header header) {
    printf("%-20s magic: %s   nfat_arch: %d\n", "FAT_HEADER", stringifyMagic(magic).c_str(), header.nfat_arch);
}

static void printFatArchs(struct fat_arch *archs, int nfat_arch) {
    for (int i = 0; i < nfat_arch; ++i) {
        struct fat_arch arch = archs[i];

        printf("#%d: cputype: %-10s  cpusubtype: %-8s   offset: %-8d size: %d\n",
            i, stringifyCPUType(arch.cputype).c_str(), stringifyCPUSubType(arch.cputype, arch.cpusubtype).c_str(), arch.offset, arch.size);
    }
    printf("\n");
}

static void printMachHeader(struct mach_header_64 header) {
    printf("%-20s magic: %s   cputype: %s   cpusubtype: %s   filetype: %s   ncmds: %d   sizeofcmds: %d   \n%-20s flags: %s\n",
        "MACHO_HEADER",
        stringifyMagic(header.magic).c_str(),
        stringifyCPUType(header.cputype).c_str(),
        stringifyCPUSubType(header.cputype, header.cpusubtype).c_str(),
        stringifyFileType(header.filetype).c_str(),
        header.ncmds, header.sizeofcmds, "",
        stringifyHeaderFlags(header.flags).c_str());
}

static std::string stringifyMagic(uint32_t magic) {
    switch (magic) {
        case FAT_MAGIC:     return std::string("FAT_MAGIC");
        case FAT_CIGAM:     return std::string("FAT_CIGAM");
        case FAT_MAGIC_64:  return std::string("FAT_MAGIC_64");
        case FAT_CIGAM_64:  return std::string("FAT_CIGAM_64");
        case MH_MAGIC:      return std::string("MH_MAGIC");
        case MH_CIGAM:      return std::string("MH_CIGAM");
        case MH_MAGIC_64:   return std::string("MH_MAGIC_64");
        case MH_CIGAM_64:   return std::string("MH_CIGAM_64");
        default: {
            std::stringstream ss;
            ss << "0x" << std::hex << magic;
            return ss.str();
        }
    }
}

static std::string stringifyCPUType(cpu_type_t cputype) {
    switch (cputype) {
        case CPU_TYPE_X86:      return std::string("X86");
        case CPU_TYPE_X86_64:   return std::string("X86_64");
        case CPU_TYPE_ARM:      return std::string("ARM");
        case CPU_TYPE_ARM64:    return std::string("ARM64");
        default: {
            std::stringstream ss;
            ss << "0x" << std::hex << cputype;
            return ss.str();
        }
    }
}

static std::string stringifyCPUSubType(cpu_type_t cputype,  cpu_subtype_t cpusubtype) {
    if (cputype == CPU_TYPE_ARM64) {
        if (cpusubtype == CPU_SUBTYPE_ARM64_ALL) {
            return std::string("ALL");
        } else if (cpusubtype == (CPU_SUBTYPE_ARM64E | CPU_SUBTYPE_PTRAUTH_ABI)) {
            return std::string("E");
        }

    } else if (cputype == CPU_TYPE_X86_64) {
        if (cpusubtype == CPU_SUBTYPE_X86_64_ALL) {
            return std::string("ALL");
        }
    }

    std::stringstream ss;
    ss << "0x" << std::hex << cpusubtype;
    return ss.str();
}


static std::string stringifyFileType(uint32_t filetype) {
    switch (filetype) {
        case MH_OBJECT:     return std::string("OBJECT");
        case MH_EXECUTE:    return std::string("EXECUTE");
        case MH_DYLIB:      return std::string("DYLIB");
        case MH_DYLINKER:   return std::string("DYLINKER");
        case MH_BUNDLE:     return std::string("BUNDLE");
        case MH_DSYM:       return std::string("DSYM");
        default: {
            std::stringstream ss;
            ss << "0x" << std::hex << filetype;
            return ss.str();
        }
    }
}

static std::string stringifyHeaderFlags(uint32_t flags) {
    std::vector<std::string> flagArray;

    std::map<int, std::string> flagMap = {
        {MH_NOUNDEFS, std::string("NOUNDEFS")},
        {MH_INCRLINK, std::string("INCRLINK")},
        {MH_DYLDLINK, std::string("DYLDLINK")},
        {MH_BINDATLOAD, std::string("BINDATLOAD")},
        {MH_PREBOUND, std::string("PREBOUND")},
        {MH_SPLIT_SEGS, std::string("SPLIT_SEGS")},
        {MH_LAZY_INIT, std::string("LAZY_INIT")},
        {MH_TWOLEVEL, std::string("TWOLEVEL")},
        {MH_FORCE_FLAT, std::string("FORCE_FLAT")},
        {MH_NOMULTIDEFS, std::string("NOMULTIDEFS")},
        {MH_NOFIXPREBINDING, std::string("NOFIXPREBINDING")},
        {MH_PREBINDABLE, std::string("PREBINDABLE")},
        {MH_ALLMODSBOUND, std::string("ALLMODSBOUND")},
        {MH_SUBSECTIONS_VIA_SYMBOLS, std::string("SUBSECTIONS_VIA_SYMBOLS")},
        {MH_CANONICAL, std::string("CANONICAL")},
        {MH_WEAK_DEFINES, std::string("WEAK_DEFINES")},
        {MH_BINDS_TO_WEAK, std::string("BINDS_TO_WEAK")},
        {MH_ALLOW_STACK_EXECUTION, std::string("ALLOW_STACK_EXECUTION")},
        {MH_ROOT_SAFE, std::string("ROOT_SAFE")},
        {MH_SETUID_SAFE, std::string("SETUID_SAFE")},
        {MH_NO_REEXPORTED_DYLIBS, std::string("NO_REEXPORTED_DYLIBS")},
        {MH_PIE, std::string("PIE")},
        {MH_DEAD_STRIPPABLE_DYLIB, std::string("STRIPPABLE_DYLIB")},
        {MH_HAS_TLV_DESCRIPTORS, std::string("HAS_TLV_DESCRIPTORS")},
        {MH_NO_HEAP_EXECUTION, std::string("NO_HEAP_EXECUTION")},
        {MH_APP_EXTENSION_SAFE, std::string("APP_EXTENSION_SAFE")},
        {MH_NLIST_OUTOFSYNC_WITH_DYLDINFO, std::string("NLIST_OUTOFSYNC_WITH_DYLDINFO")},
        {MH_SIM_SUPPORT, std::string("SIM_SUPPORT")},
        {MH_DYLIB_IN_CACHE, std::string("DYLIB_IN_CACHE")},
    };

    for (auto entry : flagMap) {
        if (flags & entry.first) {
            flagArray.push_back(entry.second);
        }
    }

    std::stringstream ss;
    for (auto flag: flagArray) {
        ss << flag << " ";
    }

    return ss.str();

}
