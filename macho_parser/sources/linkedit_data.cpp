#include <string.h>

#include "utils/utils.h"
#include "load_command.h"
#include "argument.h"
#include "exports_trie.h"
#include "symtab.h"

// code_signature.cpp
void printCodeSignature(uint8_t *base, uint32_t dataoff, uint32_t datasize);

// chained_fixups.cpp
void printChainedFixups(uint8_t *base, uint32_t dataoff, uint32_t datasize);

static std::string formatCommandName(uint32_t cmd);
static void printFunctionStarts(uint8_t *base, uint32_t dataoff, uint32_t datasize);

void printLinkEditData(uint8_t *base, struct linkedit_data_command *linkEditDataCmd) {
    printf("%-20s cmdsize: %-6u dataoff: 0x%x (%d)   datasize: %d\n",
        formatCommandName(linkEditDataCmd->cmd).c_str(), linkEditDataCmd->cmdsize,
        linkEditDataCmd->dataoff, linkEditDataCmd->dataoff, linkEditDataCmd->datasize);

    if (args.verbosity == 0) { return; }

    if (linkEditDataCmd->cmd == LC_FUNCTION_STARTS) {
        printFunctionStarts(base, linkEditDataCmd->dataoff, linkEditDataCmd->datasize);
    } else if (linkEditDataCmd->cmd == LC_DYLD_CHAINED_FIXUPS) {
        printChainedFixups(base, linkEditDataCmd->dataoff, linkEditDataCmd->datasize);
    } else if (linkEditDataCmd->cmd == LC_DYLD_EXPORTS_TRIE) {
        printExportTrie(base, linkEditDataCmd->dataoff, linkEditDataCmd->datasize);
    } else if (linkEditDataCmd->cmd == LC_CODE_SIGNATURE) {
        printCodeSignature(base, linkEditDataCmd->dataoff, linkEditDataCmd->datasize);
    }
}

static std::string formatCommandName(uint32_t cmd) {
    switch (cmd) {
    case LC_CODE_SIGNATURE: return "LC_CODE_SIGNATURE";
    case LC_SEGMENT_SPLIT_INFO: return "LC_SEGMENT_SPLIT_INFO";
    case LC_FUNCTION_STARTS: return "LC_FUNCTION_STARTS";
    case LC_DATA_IN_CODE: return "LC_DATA_IN_CODE";
    case LC_DYLIB_CODE_SIGN_DRS: return "LC_DYLIB_CODE_SIGN_DRS";
    case LC_LINKER_OPTIMIZATION_HINT: return "LC_LINKER_OPTIMIZATION_HINT";
    case LC_DYLD_EXPORTS_TRIE: return "LC_DYLD_EXPORTS_TRIE";
    case LC_DYLD_CHAINED_FIXUPS: return "LC_DYLD_CHAINED_FIXUPS";
#if __clang_major__ >= 15
    case LC_ATOM_INFO: return "LC_ATOM_INFO";
#endif
    default: return "UNKNOWN";
    }
}

static bool textSegmentLoadCommand(struct load_command *lcmd) {
    return lcmd->cmd == LC_SEGMENT_64 && strcmp(((struct segment_command_64 *)lcmd)->segname, "__TEXT") == 0;
}

static bool symtabLoadCommand(struct load_command *lcmd) {
    return lcmd->cmd == LC_SYMTAB;
}

static void printFunctionStarts(uint8_t *base, uint32_t dataoff, uint32_t datasize) {
    if (!args.verbosity) { return; }

    struct segment_command_64 *text_segment = (struct segment_command_64 *)search_load_command(base, 0, textSegmentLoadCommand).lcmd;
    struct symtab_command *symtab_cmd = (struct symtab_command *)search_load_command(base, 0, symtabLoadCommand).lcmd;

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


