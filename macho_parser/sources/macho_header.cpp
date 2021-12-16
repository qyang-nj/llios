#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <mach-o/fat.h>
#include <mach-o/swap.h>
#include <string>
#include <iomanip>
#include <sstream>

extern "C" {
#include "util.h"
#include "argument.h"
}

#include "macho_header.h"

#define NEEDS_SWAP(magic) (magic == FAT_CIGAM || magic == FAT_CIGAM_64 || magic == MH_CIGAM || magic == MH_CIGAM_64)

static uint32_t read_magic(uint8_t *base, int offset);
static struct fat_header read_fat_header(uint8_t *base, bool needs_swap);
static struct fat_arch *read_fat_archs(uint8_t *base, struct fat_header header, bool needs_swap);
static struct mach_header_64 read_mach_header(uint8_t *base, uint64_t offset);

static void print_fat_header(uint32_t magic, struct fat_header header);
static void print_fat_archs(struct fat_arch *archs, int nfat_arch);
static void print_mach_header(struct mach_header_64 header);

static void format_magic(uint32_t magic, char *name);
static void format_file_type(uint32_t filetype, char *name);

static std::string stringifyCPUType(cpu_type_t cputype);
static std::string stringifyCPUSubType(cpu_type_t cputype,  cpu_subtype_t cpusubtype);

struct mach_header_64 *parseMachHeader(uint8_t *base) {
    uint32_t magic = read_magic(base, 0);
    int mach_header_offset = 0;
    const char *cpuType;

    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        struct fat_header header = read_fat_header(base, NEEDS_SWAP(magic));
        struct fat_arch *fat_archs = read_fat_archs(base, header, NEEDS_SWAP(magic));

        if (show_header()) {
            print_fat_header(magic, header);
            print_fat_archs(fat_archs, header.nfat_arch);
        }

        for (int i = 0; i < header.nfat_arch; ++i) {
            cpuType = stringifyCPUType(fat_archs[i].cputype).c_str();
            if ((fat_archs[i].cputype & CPU_ARCH_ABI64) && is_selected_arch(cpuType)) {
                mach_header_offset = fat_archs[i].offset;
                break;
            }
        }

        free(fat_archs);

        if (mach_header_offset == 0) {
            if (args.arch != NULL) {
                fprintf (stderr, "The binary doesn't contain %s architecture.\n", args.arch);
            } else {
                fprintf (stderr, "The binary doesn't contain any 64-bit architecture.\n");
            }
            exit(1);
        }
    }

    magic = read_magic(base, mach_header_offset);
    if (magic != MH_MAGIC_64) {
        char magic_name[32];
        format_magic(magic, magic_name);
        fprintf (stderr, "Magic %s is not recognized or supported.\n", magic_name);
        exit(1);
    }

    struct mach_header_64 header = read_mach_header(base, mach_header_offset);
    if (mach_header_offset == 0) { // non-fat binary
        cpuType = stringifyCPUType(header.cputype).c_str();
        if (!is_selected_arch(cpuType)) {
            fprintf (stderr, "The binary doesn't contain %s architecture.\n", args.arch);
            exit(1);
        }
    }

    if (show_header()) {
        print_mach_header(header);
    }

    return (struct mach_header_64 *)(base + mach_header_offset);
}

static uint32_t read_magic(uint8_t *filebase, int offset) {
    uint32_t magic = *(uint32_t *)(filebase + offset);
    return magic;
}

static struct fat_header read_fat_header(uint8_t *base, bool needs_swap) {
    struct fat_header header = *(struct fat_header *)base;
    if (needs_swap) {
        swap_fat_header(&header, NX_UnknownByteOrder);
    }
    return header;
}

static struct fat_arch *read_fat_archs(uint8_t *base, struct fat_header header, bool needs_swap) {
    size_t archs_size = sizeof(struct fat_arch) * header.nfat_arch;
    struct fat_arch *archs = (struct fat_arch *)malloc(archs_size);
    memcpy(archs, base + sizeof(header), archs_size);

    if (needs_swap) {
        swap_fat_arch(archs, header.nfat_arch, NX_UnknownByteOrder);
    }
    return archs;
}

static struct mach_header_64 read_mach_header(uint8_t *base, uint64_t offset) {
    struct mach_header_64 header = *(struct mach_header_64 *)(base + offset);
    return header;
}

static void print_fat_header(uint32_t magic, struct fat_header header) {
    char magic_name[32];
    format_magic(magic, magic_name);
    printf("%-20s magic: %s   nfat_arch: %d\n", "FAT_HEADER", magic_name, header.nfat_arch);
}

static void print_fat_archs(struct fat_arch *archs, int nfat_arch) {
    for (int i = 0; i < nfat_arch; ++i) {
        struct fat_arch arch = archs[i];

        printf("#%d: cputype: %-10s  cpusubtype: %-8s   offset: %-8d size: %d\n",
            i, stringifyCPUType(arch.cputype).c_str(), stringifyCPUSubType(arch.cputype, arch.cpusubtype).c_str(), arch.offset, arch.size);
    }
    printf("\n");
}

static void print_mach_header(struct mach_header_64 header) {
    char magic_name[32];
    char file_type[64];

    format_magic(header.magic, magic_name);
    format_file_type(header.filetype, file_type);

    printf("%-20s magic: %s   cputype: %s   cpusubtype: %s   filetype: %s   ncmds: %d   sizeofcmds: %d   flags: 0x%X\n",
        "MACHO_HEADER", magic_name, stringifyCPUType(header.cputype).c_str(), stringifyCPUSubType(header.cputype, header.cpusubtype).c_str(),
        file_type, header.ncmds, header.sizeofcmds, header.flags);
}

static void format_magic(uint32_t magic, char *name) {
    switch (magic) {
        case FAT_MAGIC:     strcpy(name, "FAT_MAGIC");      break;
        case FAT_CIGAM:     strcpy(name, "FAT_CIGAM");      break;
        case FAT_MAGIC_64:  strcpy(name, "FAT_MAGIC_64");   break;
        case FAT_CIGAM_64:  strcpy(name, "FAT_CIGAM_64");   break;
        case MH_MAGIC:      strcpy(name, "MH_MAGIC");       break;
        case MH_CIGAM:      strcpy(name, "MH_CIGAM");       break;
        case MH_MAGIC_64:   strcpy(name, "MH_MAGIC_64");    break;
        case MH_CIGAM_64:   strcpy(name, "MH_CIGAM_64");    break;
        default:            sprintf(name, "%x", magic);
    }
}

static void format_file_type(uint32_t filetype, char *name) {
    switch (filetype) {
        case MH_OBJECT:     strcpy(name, "MH_OBJECT");      break;
        case MH_EXECUTE:    strcpy(name, "MH_EXECUTE");     break;
        case MH_DYLIB:      strcpy(name, "MH_DYLIB");       break;
        case MH_DYLINKER:   strcpy(name, "MH_DYLINKER");    break;
        case MH_BUNDLE:     strcpy(name, "MH_BUNDLE");      break;
        case MH_DSYM:       strcpy(name, "MH_DSYM");        break;
        default:            sprintf(name, "0x%x", filetype);
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
