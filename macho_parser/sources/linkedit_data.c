#include <stdio.h>
#include <mach-o/fixup-chains.h>
#include <sys/mman.h>

#include "util.h"
#include "argument.h"

#include "linkedit_data.h"

static char *command_name(uint32_t cmd);
static void parse_function_starts(FILE *fptr, uint32_t dataoff, uint32_t datasize);
static void parse_chained_fixups(FILE *fptr, uint32_t dataoff, uint32_t datasize);

void parse_linkedit_data(FILE *fptr, struct linkedit_data_command *linkedit_data_cmd) {
    printf("%-20s cmdsize: %-6u dataoff: 0x%x (%d)   datasize: %d\n",
        command_name(linkedit_data_cmd->cmd), linkedit_data_cmd->cmdsize,
        linkedit_data_cmd->dataoff, linkedit_data_cmd->dataoff, linkedit_data_cmd->datasize);

    if (args.short_desc) { return; }

    if (linkedit_data_cmd->cmd == LC_FUNCTION_STARTS) {
        parse_function_starts(fptr, linkedit_data_cmd->dataoff, linkedit_data_cmd->datasize);
    } else if (linkedit_data_cmd->cmd == LC_DYLD_CHAINED_FIXUPS) {
        parse_chained_fixups(fptr, linkedit_data_cmd->dataoff, linkedit_data_cmd->datasize);
    }
}

static char *command_name(uint32_t cmd) {
    char *cmd_name = "";
    switch (cmd)
    {
    case LC_CODE_SIGNATURE:
        cmd_name = "LC_CODE_SIGNATURE";
        break;
    case LC_SEGMENT_SPLIT_INFO:
        cmd_name = "LC_SEGMENT_SPLIT_INFO";
        break;
    case LC_FUNCTION_STARTS:
        cmd_name = "LC_FUNCTION_STARTS";
        break;
    case LC_DATA_IN_CODE:
        cmd_name = "LC_DATA_IN_CODE";
        break;
    case LC_DYLIB_CODE_SIGN_DRS:
        cmd_name = "LC_DYLIB_CODE_SIGN_DRS";
        break;
    case LC_LINKER_OPTIMIZATION_HINT:
        cmd_name = "LC_LINKER_OPTIMIZATION_HINT";
        break;
    case LC_DYLD_EXPORTS_TRIE:
        cmd_name = "LC_DYLD_EXPORTS_TRIE";
        break;
    case LC_DYLD_CHAINED_FIXUPS:
        cmd_name = "LC_DYLD_CHAINED_FIXUPS";
        break;
    default:
        break;
    }
    return cmd_name;
}

static void parse_function_starts(FILE *fptr, uint32_t dataoff, uint32_t datasize) {
    if (!args.verbose) { return; }

    uint8_t *func_starts = load_bytes(fptr, dataoff, datasize);

    int i = 0;
    int address = 0;
    while(func_starts[i] != 0 && i < datasize) {
        uint64_t num = 0;
        i += read_uleb128(func_starts + i, &num);

        address += num;
        printf("    0x%x\n", address);
    }

    free(func_starts);
}


static void parse_chained_fixups(FILE *fptr, uint32_t dataoff, uint32_t datasize) {
    void *base_addr = mmap(NULL, datasize, PROT_READ, MAP_PRIVATE, fileno(fptr), dataoff);

    printf("sizeof(dyld_chained_fixups_header): %lu\n", sizeof(struct dyld_chained_fixups_header));
    struct dyld_chained_fixups_header *header = base_addr; //load_bytes(fptr, dataoff, sizeof(struct dyld_chained_fixups_header));
    printf("fixups_version: %d\n", header->fixups_version);
    printf("starts_offset: %d\n", header->starts_offset);
    printf("imports_offset: %d\n", header->imports_offset);
    printf("symbols_offset: %d\n", header->symbols_offset);
    printf("imports_count: %d\n", header->imports_count);
    printf("imports_format: %d\n", header->imports_format);
    printf("symbols_format: %d\n", header->symbols_format);
    printf("\n");

    printf("sizeof(dyld_chained_starts_in_image): %lu\n", sizeof(struct dyld_chained_starts_in_image));
    struct dyld_chained_starts_in_image *starts_in_image = base_addr + header->starts_offset;
    printf("seg_count: %d\n", starts_in_image->seg_count);
    printf("seg_info_offset: %d\n", starts_in_image->seg_info_offset[0]);

    uint32_t *offsets = starts_in_image->seg_info_offset;
    for (int i = 0; i < starts_in_image->seg_count; ++i) {
        printf("    [%d] %d\n", i, offsets[i]);

        if (offsets[i] == 0) { continue; }

        struct dyld_chained_starts_in_segment* starts_in_segment = load_bytes(fptr, dataoff + header->starts_offset + offsets[i], sizeof(struct dyld_chained_starts_in_segment));
        printf("        size: %d\n", starts_in_segment->size);
        printf("        page_size: 0x%x\n", starts_in_segment->page_size);
        printf("        pointer_format: %d\n", starts_in_segment->pointer_format);
        printf("        segment_offset: 0x%llx\n", starts_in_segment->segment_offset);
        printf("        max_valid_pointer: %d\n", starts_in_segment->max_valid_pointer);
        printf("        page_count: %d\n", starts_in_segment->page_count);
        printf("        page_start: %d\n", starts_in_segment-> page_start[0]);

        uint16_t *page_starts = starts_in_segment-> page_start;
        for (int j = 0; j < starts_in_segment->page_count; ++j) {
            printf("            [%d] %d\n", j, page_starts[j]);

            if (page_starts[j] == DYLD_CHAINED_PTR_START_NONE) { continue; }

            uint16_t chain = starts_in_segment->segment_offset + starts_in_segment->page_size * j + page_starts[j];

            bool done = false;
            while (!done) {
                if (starts_in_segment->pointer_format == DYLD_CHAINED_PTR_64) {
                    struct dyld_chained_ptr_64_bind bind;
                    read_bytes(fptr, chain, &bind, sizeof(struct dyld_chained_ptr_64_bind));
                    if (bind.bind) {
                        printf("-- 0x%08x bind   > next: %-3d   ordinal: %d   addend: %d    reserved: %d\n",
                            chain, bind.next, bind.ordinal, bind.addend, bind.reserved);
                    } else {
                        // rebase
                        struct dyld_chained_ptr_64_rebase rebase = *(struct dyld_chained_ptr_64_rebase *)&bind;
                        printf("-- 0x%08x rebase > next: %-3d   target: 0x%llx   high8: %d\n", chain, rebase.next, rebase.target, rebase.high8);
                    }

                    if (bind.next == 0) {
                        done = true;
                    } else {
                        chain += bind.next * 4;
                    }

                } else {
                    printf("Other pointer format: 0x%x", starts_in_segment->pointer_format);
                }
            }
        }
    }

    munmap(base_addr, datasize);
}
