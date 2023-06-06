#include <string.h>

#include "utils/utils.h"
#include "load_command.h"
#include "argument.h"
#include "chained_fixups.h"
#include "exports_trie.h"
#include "symtab.h"
#include "code_signature.h"

#include "linkedit_data.h"

static char *command_name(uint32_t cmd);
static void parse_function_starts(void *base, uint32_t dataoff, uint32_t datasize);

void parse_linkedit_data(void *base, struct linkedit_data_command *linkedit_data_cmd) {
    printf("%-20s cmdsize: %-6u dataoff: 0x%x (%d)   datasize: %d\n",
        command_name(linkedit_data_cmd->cmd), linkedit_data_cmd->cmdsize,
        linkedit_data_cmd->dataoff, linkedit_data_cmd->dataoff, linkedit_data_cmd->datasize);

    if (args.verbosity == 0) { return; }

    if (linkedit_data_cmd->cmd == LC_FUNCTION_STARTS) {
        parse_function_starts(base, linkedit_data_cmd->dataoff, linkedit_data_cmd->datasize);
    } else if (linkedit_data_cmd->cmd == LC_DYLD_CHAINED_FIXUPS) {
        printChainedFixups(base, linkedit_data_cmd->dataoff, linkedit_data_cmd->datasize);
    } else if (linkedit_data_cmd->cmd == LC_DYLD_EXPORTS_TRIE) {
        printExportTrie(base, linkedit_data_cmd->dataoff, linkedit_data_cmd->datasize);
    } else if (linkedit_data_cmd->cmd == LC_CODE_SIGNATURE) {
        printCodeSignature(base, linkedit_data_cmd->dataoff, linkedit_data_cmd->datasize);
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
#if __clang_major__ >= 15
    case LC_ATOM_INFO:
        cmd_name = "LC_ATOM_INFO";
#endif
        break;
    default:
        break;
    }
    return cmd_name;
}

static bool text_segment_load_command(struct load_command *lcmd) {
    return lcmd->cmd == LC_SEGMENT_64 && strcmp(((struct segment_command_64 *)lcmd)->segname, "__TEXT") == 0;
}

static bool symtab_load_command(struct load_command *lcmd) {
    return lcmd->cmd == LC_SYMTAB;
}

static void parse_function_starts(void *base, uint32_t dataoff, uint32_t datasize) {
    if (!args.verbosity) { return; }

    struct segment_command_64 *text_segment = (struct segment_command_64 *)search_load_command(base, 0, text_segment_load_command).lcmd;
    struct symtab_command *symtab_cmd = (struct symtab_command *)search_load_command(base, 0, symtab_load_command).lcmd;

    uint8_t *func_starts = base + dataoff;

    int i = 0;
    uint64_t address = text_segment->vmaddr;
    int count = 0;
    while(func_starts[i] != 0 && i < datasize) {
        if (count > 10 && !args.no_truncate) {
            printf("    ... more ...\n");
            break;
        }

        uint64_t num = 0;
        i += readULEB128(func_starts + i, &num);
        address += num;

        char *symbol = lookup_symbol_by_address(address, base, symtab_cmd);
        printf("  %#llx  %s\n", address, symbol);

        count++;
    }
}


