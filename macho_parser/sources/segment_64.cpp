#include <string.h>
#include <stdio.h>
#include <string>
#include <iostream>
#include <sstream>
#include <bitset>
#include <iomanip>

#include "argument.h"
#include "util.h"
#include "load_command.h"
#include "symtab.h"

// llvm_cov.cpp
void printCovMapSection(uint8_t *sectBase, size_t sectSize);
void printCovFunSection(uint8_t *sectBase, size_t sectSize);
void printPrfNamesSection(uint8_t *sectBase, size_t sectSize);

static bool hasSectionToShow(struct segment_command_64 *segCmd, int firstSectionIndex);
static void printSection(uint8_t *base, struct section_64 sect, int sectionIndex);
static void printCStringSection(uint8_t *sectBase, size_t sectSize);
static void printPointerSection(uint8_t *base, struct section_64 *sect);

static std::string formatSectionType(uint8_t type);

void printSegment(uint8_t *base, struct segment_command_64 *segCmd, int firstSectionIndex) {
    if (hasSectionSpecifed() && !hasSectionToShow(segCmd, firstSectionIndex)) {
        // If --section is specified and no section needs to be show in this segment, just return.
        return;
    }

    char formatted_filesize[16];
    char formatted_vmsize[16];

    format_size(segCmd->filesize, formatted_filesize);
    format_size(segCmd->vmsize, formatted_vmsize);

    printf("%-20s cmdsize: %-6d segname: %-12.16s   file: 0x%08llx-0x%08llx %-9s  vm: 0x%09llx-0x%09llx %-9s prot: %d/%d\n",
        "LC_SEGMENT_64", segCmd->cmdsize, segCmd->segname,
        segCmd->fileoff, segCmd->fileoff + segCmd->filesize, formatted_filesize,
        segCmd->vmaddr, segCmd->vmaddr + segCmd->vmsize, formatted_vmsize,
        segCmd->initprot, segCmd->maxprot);

    if (args.verbosity < 1) {
        return;
    }

    // section_64 is immediately after segment_command_64.
    struct section_64 *sections = (struct section_64 *)((uint8_t *)segCmd + sizeof(struct segment_command_64));

    for (int i = 0; i < segCmd->nsects; ++i) {
        struct section_64 sect = sections[i];
        if (showSection(firstSectionIndex + i, sect.sectname)) {
            printSection(base, sect, firstSectionIndex + i);
        }
    }
}

static bool hasSectionToShow(struct segment_command_64 *segCmd, int firstSectionIndex) {
    struct section_64 *sections = (struct section_64 *)((uint8_t *)segCmd + sizeof(struct segment_command_64));
    for (int i = 0; i < segCmd->nsects; ++i) {
        struct section_64 sect = sections[i];
        if (showSection(firstSectionIndex + i, sect.sectname)) {
            return true;
        }
    }
    return false;
}

static void printSection(uint8_t *base, struct section_64 sect, int sectionIndex) {
    char formattedSegSec[64];
    char formattedSize[16];

    const uint8_t type = sect.flags & SECTION_TYPE;

    auto formattedType = formatSectionType(type);
    snprintf(formattedSegSec, sizeof(formattedSegSec), "(%.16s,%.16s)", sect.segname, sect.sectname);
    format_size(sect.size, formattedSize);

    printf("  %2d: 0x%09x-0x%09llx %-11s %-32s  type: %s  offset: %d",
        sectionIndex, sect.offset, sect.offset + sect.size, formattedSize, formattedSegSec, formattedType.c_str(), sect.offset);

    if (sect.reserved1 > 0) {
        printf("   reserved1: %2d", sect.reserved1);
    }

    if (sect.reserved2 > 0) {
        printf("   reserved1: %2d", sect.reserved2);
    }

    printf("\n");

    if (args.verbosity < 2) {
        return;
    }

    if (strncmp(sect.sectname, "__llvm_covmap", 16) == 0) {
        printCovMapSection(base + sect.offset, sect.size);
    } else if (strncmp(sect.sectname, "__llvm_covfun", 16) == 0) {
        printCovFunSection(base + sect.offset, sect.size);
    } else if (strncmp(sect.sectname, "__llvm_prf_names", 16) == 0) {
        printPrfNamesSection(base + sect.offset, sect.size);
    } else if (type == S_CSTRING_LITERALS) {
        // (__TEXT,__cstring), (__TEXT,__objc_classname__TEXT), (__TEXT,__objc_methname), etc..
        printCStringSection(base + sect.offset, sect.size);
    } else if (type == S_MOD_INIT_FUNC_POINTERS
        || type == S_NON_LAZY_SYMBOL_POINTERS
        || type == S_LAZY_SYMBOL_POINTERS) {
        // (__DATA_CONST,__mod_init_func)
        printPointerSection(base, &sect);
    }
}

static void printCStringSection(uint8_t *sectBase, size_t sectSize) {
    char *formatted = (char *)malloc(1024 * 10);

    int count = 0;
    char *ptr = (char *)sectBase;
    while(ptr < (char *)(sectBase + sectSize)) {
        if (strlen(ptr) > 0) {
            format_string(ptr, formatted);
            printf("    \"%s\"\n", formatted);
            ptr += strlen(ptr);

            if (count >= 10 && !args.no_truncate) {
                break;
            }
            count += 1;
        }
        ptr += 1;
    }

    free(formatted);

    if (!args.no_truncate && ptr < (char *)(sectBase + sectSize)) {
        printf("    ... more ...\n");
    }
}

static void printPointerSection(uint8_t *base, struct section_64 *sect) {
    void *section = base + sect->offset;

    const size_t count = sect->size / sizeof(uintptr_t);
    int max_count = args.no_truncate ? count : MIN(count, 10);

    struct symtab_command *symtab_cmd = (struct symtab_command *)search_load_command(base, 0, is_symtab_load_command).lcmd;

    for (int i = 0; i < max_count; ++i) {
        char *symbol = lookup_symbol_by_address(*((uintptr_t *)section + i), base, symtab_cmd);
        printf("    0x%lx  %s\n", *((uintptr_t *)section + i), (symbol == NULL ? "" : symbol));
    }

    if (!args.no_truncate && count > 10) {
        printf("    ... %lu more ...\n", count - 10);
    }
}

static std::string formatSectionType(uint8_t type) {
    if (type == S_REGULAR) {
       return std::string("S_REGULAR");
    } else if (type == S_ZEROFILL) {
        return std::string("S_ZEROFILL");
    } else if (type == S_CSTRING_LITERALS) {
        return std::string("S_CSTRING_LITERALS");
    } else if (type == S_4BYTE_LITERALS) {
        return std::string("S_4BYTE_LITERALS");
    } else if (type == S_8BYTE_LITERALS) {
        return std::string("S_8BYTE_LITERALS");
    } else if (type == S_16BYTE_LITERALS) {
        return std::string("S_16BYTE_LITERALS");
    } else if (type == S_LITERAL_POINTERS) {
        return std::string("S_LITERAL_POINTERS");
    } else if (type == S_NON_LAZY_SYMBOL_POINTERS) {
        return std::string("S_NON_LAZY_SYMBOL_POINTERS");
    } else if (type == S_LAZY_SYMBOL_POINTERS) {
        return std::string("S_LAZY_SYMBOL_POINTERS");
    } else if (type == S_LITERAL_POINTERS) {
        return std::string("S_LITERAL_POINTERS");
    } else if (type == S_SYMBOL_STUBS) {
        return std::string("S_SYMBOL_STUBS");
    } else if (type == S_MOD_INIT_FUNC_POINTERS) {
        return std::string("S_MOD_INIT_FUNC_POINTERS");
    } else if (type == S_THREAD_LOCAL_ZEROFILL) {
        return std::string("S_THREAD_LOCAL_ZEROFILL");
    } else if (type == S_THREAD_LOCAL_VARIABLES) {
        return std::string("S_THREAD_LOCAL_VARIABLES");
    } else if (type == S_COALESCED) {
        return std::string("S_COALESCED");
    } else {
        std::stringstream sstream;
        sstream << "0x" << std::setfill('0') << std::setw(2) << std::hex << (int)type;
        return std::string("OTHER(") + sstream.str() + ")";
    }
}
