#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <mach-o/loader.h>
#include <mach-o/fixup-chains.h>
#include <sys/mman.h>

#include "util.h"

#include "chained_fixups.h"

static void print_chained_fixups_header(struct dyld_chained_fixups_header *header);
static void print_imports(struct dyld_chained_fixups_header *header);

static void format_pointer_format(uint16_t pointer_format, char *formatted);

void parse_chained_fixups(uint8_t *base, uint32_t dataoff, uint32_t datasize) {
    uint8_t *fixup_base = base + dataoff;

    struct dyld_chained_fixups_header *header = (struct dyld_chained_fixups_header *)fixup_base;
    print_chained_fixups_header(header);
    print_imports(header);

    struct dyld_chained_starts_in_image *starts_in_image = (struct dyld_chained_starts_in_image *)(fixup_base + header->starts_offset);
    // printf("    STARTS IN IMAGE\n");
    // printf("    seg_count: %d\n", starts_in_image->seg_count);
    // printf("    seg_info_offset: %d\n", starts_in_image->seg_info_offset[0]);
    // printf("\n");

    uint32_t *offsets = starts_in_image->seg_info_offset;
    for (int i = 0; i < starts_in_image->seg_count; ++i) {
        printf("    SEGMENT %d (offset: %d)\n", i, offsets[i]);

        if (offsets[i] == 0) {
            printf("\n");
            continue;
        }

        struct dyld_chained_starts_in_segment* starts_in_segment = (struct dyld_chained_starts_in_segment*)(base + dataoff + header->starts_offset + offsets[i]);
        char formatted_pointer_format[256];
        format_pointer_format(starts_in_segment->pointer_format, formatted_pointer_format);

        printf("    size: %d\n", starts_in_segment->size);
        printf("    page_size: 0x%x\n", starts_in_segment->page_size);
        printf("    pointer_format: %d (%s)\n", starts_in_segment->pointer_format, formatted_pointer_format);
        printf("    segment_offset: 0x%llx\n", starts_in_segment->segment_offset);
        printf("    max_valid_pointer: %d\n", starts_in_segment->max_valid_pointer);
        printf("    page_count: %d\n", starts_in_segment->page_count);
        printf("    page_start: %d\n", starts_in_segment-> page_start[0]);

        uint16_t *page_starts = starts_in_segment-> page_start;
        for (int j = 0; j < starts_in_segment->page_count; ++j) {
            printf("        SEGMENT %d, PAGE %d (offset: %d)\n", i, j, page_starts[j]);

            if (page_starts[j] == DYLD_CHAINED_PTR_START_NONE) { continue; }

            uint32_t chain = starts_in_segment->segment_offset + starts_in_segment->page_size * j + page_starts[j];

            bool done = false;
            while (!done) {
                if (starts_in_segment->pointer_format == DYLD_CHAINED_PTR_64
                    || starts_in_segment->pointer_format == DYLD_CHAINED_PTR_64_OFFSET) {
                    struct dyld_chained_ptr_64_bind bind = *(struct dyld_chained_ptr_64_bind *)(base + chain);
                    if (bind.bind) {
                        struct dyld_chained_import import = ((struct dyld_chained_import *)(fixup_base + header->imports_offset))[bind.ordinal];
                        char *symbol = (char *)(fixup_base + header->symbols_offset + import.name_offset);
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
                    printf("Unsupported pointer format: 0x%x", starts_in_segment->pointer_format);
                    break;
                }
            }
            printf("\n");
        }
    }
}

static void print_chained_fixups_header(struct dyld_chained_fixups_header *header) {
    const char *imports_format = NULL;
    switch (header->imports_format) {
        case DYLD_CHAINED_IMPORT: imports_format = "DYLD_CHAINED_IMPORT"; break;
        case DYLD_CHAINED_IMPORT_ADDEND: imports_format = "DYLD_CHAINED_IMPORT_ADDEND"; break;
        case DYLD_CHAINED_IMPORT_ADDEND64: imports_format = "DYLD_CHAINED_IMPORT_ADDEND64"; break;
    }


    printf("    CHAINED FIXUPS HEADER\n");
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

static void print_imports(struct dyld_chained_fixups_header *header) {
    printf("    IMPORTS\n");

    for (int i = 0; i < header->imports_count; ++i) {
        struct dyld_chained_import import = ((struct dyld_chained_import *)((uint8_t *)header + header->imports_offset))[i];
        printf("    [%d] lib_ordinal: %d   weak_import: %d   name_offset: %d (%s)\n",
            i, import.lib_ordinal, import.weak_import, import.name_offset,
            (char *)((uint8_t *)header + header->symbols_offset + import.name_offset));
    }
    printf("\n");
}

static void format_pointer_format(uint16_t pointer_format, char *formatted) {
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
