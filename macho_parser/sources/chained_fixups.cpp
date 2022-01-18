#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <mach-o/loader.h>
#include <mach-o/fixup-chains.h>
#include <sys/mman.h>
#include <algorithm>
#include <string>

extern "C" {
#include "argument.h"
#include "util.h"
#include "dylib.h"
}

#include "macho_binary.h"

#include "chained_fixups.h"

static void printChainedFixupsHeader(struct dyld_chained_fixups_header *header);
static void printImports(struct dyld_chained_fixups_header *header);
static void printFixupsInPage(uint8_t *base, uint8_t *fixupBase, struct dyld_chained_fixups_header *header,
    struct dyld_chained_starts_in_segment *startsInSegment, int pageIndex);

static std::string getDylibName(uint16_t dylibOrdinal);
static void formatPointerFormat(uint16_t pointer_format, char *formatted);

void printChainedFixups(uint8_t *base, uint32_t dataoff, uint32_t datasize) {
    uint8_t *fixup_base = base + dataoff;

    struct dyld_chained_fixups_header *header = (struct dyld_chained_fixups_header *)fixup_base;
    printChainedFixupsHeader(header);
    printImports(header);

    struct dyld_chained_starts_in_image *starts_in_image =
        (struct dyld_chained_starts_in_image *)(fixup_base + header->starts_offset);


    uint32_t *offsets = starts_in_image->seg_info_offset;
    for (int i = 0; i < starts_in_image->seg_count; ++i) {
        struct segment_command_64 *segCmd = machoBinary.segmentCommands[i];
        printf("  SEGMENT %.16s (offset: %d)\n", segCmd->segname, offsets[i]);

        if (offsets[i] == 0) {
            printf("\n");
            continue;
        }

        struct dyld_chained_starts_in_segment* startsInSegment = (struct dyld_chained_starts_in_segment*)(fixup_base + header->starts_offset + offsets[i]);
        char formatted_pointer_format[256];
        formatPointerFormat(startsInSegment->pointer_format, formatted_pointer_format);

        printf("    size: %d\n", startsInSegment->size);
        printf("    page_size: 0x%x\n", startsInSegment->page_size);
        printf("    pointer_format: %d (%s)\n", startsInSegment->pointer_format, formatted_pointer_format);
        printf("    segment_offset: 0x%llx\n", startsInSegment->segment_offset);
        printf("    max_valid_pointer: %d\n", startsInSegment->max_valid_pointer);
        printf("    page_count: %d\n", startsInSegment->page_count);
        printf("    page_start: %d\n", startsInSegment-> page_start[0]);

        uint16_t *page_starts = startsInSegment->page_start;
        uint16_t maxPageNum = args.no_truncate ? UINT16_MAX : 10;
        int pageCount = 0;
        for (int j = 0; j < std::min(startsInSegment->page_count, maxPageNum); ++j) {
            printf("      PAGE %d (offset: %d)\n", j, page_starts[j]);

            if (page_starts[j] == DYLD_CHAINED_PTR_START_NONE) { continue; }

            printFixupsInPage(base, fixup_base, header, startsInSegment, j);

            pageCount++;
            printf("\n");
        }

        if (pageCount < startsInSegment->page_count) {
            printf("      ... %d more pages ...\n\n", startsInSegment->page_count - pageCount);
        }
    }
}

static void printChainedFixupsHeader(struct dyld_chained_fixups_header *header) {
    const char *imports_format = NULL;
    switch (header->imports_format) {
        case DYLD_CHAINED_IMPORT: imports_format = "DYLD_CHAINED_IMPORT"; break;
        case DYLD_CHAINED_IMPORT_ADDEND: imports_format = "DYLD_CHAINED_IMPORT_ADDEND"; break;
        case DYLD_CHAINED_IMPORT_ADDEND64: imports_format = "DYLD_CHAINED_IMPORT_ADDEND64"; break;
    }

    printf("  CHAINED FIXUPS HEADER\n");
    printf("    fixups_version : %d\n", header->fixups_version);
    printf("    starts_offset  : %#4x (%d)\n", header->starts_offset, header->starts_offset);
    printf("    imports_offset : %#4x (%d)\n", header->imports_offset, header->imports_offset);
    printf("    symbols_offset : %#4x (%d)\n", header->symbols_offset, header->symbols_offset);
    printf("    imports_count  : %d\n", header->imports_count);
    printf("    imports_format : %d (%s)\n", header->imports_format, imports_format);
    printf("    symbols_format : %d (%s)\n", header->symbols_format,
        (header->symbols_format == 0 ? "UNCOMPRESSED" : "ZLIB COMPRESSED"));
    printf("\n");
}

static void printImports(struct dyld_chained_fixups_header *header) {
    printf("  IMPORTS\n");

    uint32_t maxImportNum = args.no_truncate ? UINT32_MAX : 10;
    int importCount = 0;
    for (int i = 0; i < std::min(header->imports_count, maxImportNum); ++i) {
        struct dyld_chained_import import =
            ((struct dyld_chained_import *)((uint8_t *)header + header->imports_offset))[i];

        printf("    [%d] lib_ordinal: %-22s   weak_import: %d   name_offset: %d (%s)\n",
            i, getDylibName(import.lib_ordinal).c_str(), import.weak_import, import.name_offset,
            (char *)((uint8_t *)header + header->symbols_offset + import.name_offset));

        importCount++;
    }

    if (importCount < header->imports_count) {
        printf("    ... %d more imports ...\n", header->imports_count - importCount);
    }

    printf("\n");
}

static void printFixupsInPage(uint8_t *base, uint8_t *fixupBase, struct dyld_chained_fixups_header *header,
    struct dyld_chained_starts_in_segment *startsInSegment, int pageIndex) {
    uint32_t chain = startsInSegment->segment_offset + startsInSegment->page_size * pageIndex + startsInSegment->page_start[pageIndex];
    bool done = false;
    int count = 0;
    int maxNumFixup = args.no_truncate ? INT_MAX : 10;
    while (!done) {
        if (startsInSegment->pointer_format == DYLD_CHAINED_PTR_64
            || startsInSegment->pointer_format == DYLD_CHAINED_PTR_64_OFFSET) {
            struct dyld_chained_ptr_64_bind bind = *(struct dyld_chained_ptr_64_bind *)(base + chain);
            if (bind.bind) {
                struct dyld_chained_import import = ((struct dyld_chained_import *)(fixupBase + header->imports_offset))[bind.ordinal];
                char *symbol = (char *)(fixupBase + header->symbols_offset + import.name_offset);
                printf("        0x%08x BIND     ordinal: %d   addend: %d    reserved: %d   (%s)\n",
                    chain, bind.ordinal, bind.addend, bind.reserved, symbol);
            } else {
                // rebase
                struct dyld_chained_ptr_64_rebase rebase = *(struct dyld_chained_ptr_64_rebase *)&bind;
                printf("        %#010x REBASE   target: %#010llx   high8: %d\n",
                    chain, rebase.target, rebase.high8);
            }

            if (bind.next == 0) {
                done = true;
            } else {
                chain += bind.next * 4;
            }

        } else {
            printf("Unsupported pointer format: 0x%x", startsInSegment->pointer_format);
            break;
        }

        count++;
        if (count >= maxNumFixup) {
            done = true;
            printf("        ... more fixups ...\n");
        }
    }
}

static void formatPointerFormat(uint16_t pointer_format, char *formatted) {
    switch(pointer_format) {
        case DYLD_CHAINED_PTR_ARM64E: strcpy(formatted, "DYLD_CHAINED_PTR_ARM64E"); break;
        case DYLD_CHAINED_PTR_64: strcpy(formatted, "DYLD_CHAINED_PTR_64"); break;
        case DYLD_CHAINED_PTR_32: strcpy(formatted, "DYLD_CHAINED_PTR_32"); break;
        case DYLD_CHAINED_PTR_32_CACHE: strcpy(formatted, "DYLD_CHAINED_PTR_32_CACHE"); break;
        case DYLD_CHAINED_PTR_32_FIRMWARE: strcpy(formatted, "DYLD_CHAINED_PTR_32_FIRMWARE"); break;
        case DYLD_CHAINED_PTR_64_OFFSET: strcpy(formatted, "DYLD_CHAINED_PTR_64_OFFSET"); break;
        case DYLD_CHAINED_PTR_ARM64E_KERNEL: strcpy(formatted, "DYLD_CHAINED_PTR_ARM64E_KERNEL"); break;
        case DYLD_CHAINED_PTR_64_KERNEL_CACHE: strcpy(formatted, "DYLD_CHAINED_PTR_64_KERNEL_CACHE"); break;
        case DYLD_CHAINED_PTR_ARM64E_USERLAND: strcpy(formatted, "DYLD_CHAINED_PTR_ARM64E_USERLAND"); break;
        case DYLD_CHAINED_PTR_ARM64E_FIRMWARE: strcpy(formatted, "DYLD_CHAINED_PTR_ARM64E_FIRMWARE"); break;
        case DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE: strcpy(formatted, "DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE"); break;
        case DYLD_CHAINED_PTR_ARM64E_USERLAND24: strcpy(formatted, "DYLD_CHAINED_PTR_ARM64E_USERLAND24"); break;
        default: strcpy(formatted, "UNKNOWN");
    }
}

static std::string getDylibName(uint16_t dylibOrdinal) {
    std::string dylibName;

    switch (dylibOrdinal) {
        case (uint8_t)BIND_SPECIAL_DYLIB_SELF:
            dylibName = "self";
            break;
        case (uint8_t)BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE:
            dylibName = "main executable";
            break;
        case (uint8_t)BIND_SPECIAL_DYLIB_FLAT_LOOKUP:
            dylibName = "flat lookup";
            break;
        case (uint8_t)BIND_SPECIAL_DYLIB_WEAK_LOOKUP:
            dylibName = "weak lookup";
            break;
        default:
            dylibName = machoBinary.getDylibNameByOrdinal(dylibOrdinal);
    }

    return std::to_string(dylibOrdinal) + std::string(" (") + dylibName + std::string(")");
}
