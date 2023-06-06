#include <stdlib.h>
#include <mach-o/nlist.h>
#include <algorithm>

#include "load_command.h"
#include "argument.h"
#include "symtab.h"
#include "dysymtab.h"

static bool symtabLoadCommand(struct load_command *lcmd);
static void printSymbols(uint8_t *base, struct symtab_command *symtabCmd, int offset, int num);
static void printIndirectSymbols(uint8_t *base, struct symtab_command *symtabCmd, uint32_t *indirectSymtab, int size);

void printDynamicSymbolTable(uint8_t *base, struct dysymtab_command *dysymtabCmd) {
    printf("%-20s cmdsize: %-6u nlocalsym: %d  nextdefsym: %d   nundefsym: %d   nindirectsyms: %d \n",
        "LC_DYSYMTAB", dysymtabCmd->cmdsize, dysymtabCmd->nlocalsym,
        dysymtabCmd->nextdefsym,  dysymtabCmd->nundefsym, dysymtabCmd->nindirectsyms);

    if (args.verbosity == 0) { return; }

    printf("  ilocalsym     : %-10d  nlocalsym    : %d\n", dysymtabCmd->ilocalsym, dysymtabCmd->nlocalsym);
    printf("  iextdefsym    : %-10d  nextdefsym   : %d\n", dysymtabCmd->iextdefsym, dysymtabCmd->nextdefsym);
    printf("  iundefsym     : %-10d  nundefsym    : %d\n", dysymtabCmd->iundefsym, dysymtabCmd->nundefsym);
    printf("  tocoff        : 0x%-8x  ntoc         : %d\n", dysymtabCmd->tocoff, dysymtabCmd->ntoc);
    printf("  modtaboff     : 0x%-8x  nmodtab      : %d\n", dysymtabCmd->modtaboff, dysymtabCmd->nmodtab);
    printf("  extrefsymoff  : 0x%-8x  nextrefsyms  : %d\n", dysymtabCmd->extrefsymoff, dysymtabCmd->nextrefsyms);
    printf("  indirectsymoff: 0x%08x  nindirectsyms: %d\n", dysymtabCmd->indirectsymoff, dysymtabCmd->nindirectsyms);
    printf("  extreloff     : 0x%-8x  nextrel      : %d\n", dysymtabCmd->extreloff, dysymtabCmd->nextrel);
    printf("  locreloff     : 0x%-8x  nlocrel      : %d\n", dysymtabCmd->locreloff, dysymtabCmd->nlocrel);
    printf("\n");

    struct symtab_command *symtabCmd = (struct symtab_command *)search_load_command(base, 0, symtabLoadCommand).lcmd;

    if (args.show_local) {
        printf("  Local symbols (ilocalsym %d, nlocalsym:%d)\n", dysymtabCmd->ilocalsym, dysymtabCmd->nlocalsym);
        printSymbols(base, symtabCmd, dysymtabCmd->ilocalsym, dysymtabCmd->nlocalsym);
        printf("\n");
    }

    if (args.show_extdef) {
        printf("  Externally defined symbols (iextdefsym: %d, nextdefsym:%d)\n", dysymtabCmd->iextdefsym, dysymtabCmd->nextdefsym);
        printSymbols(base, symtabCmd, dysymtabCmd->iextdefsym, dysymtabCmd->nextdefsym);
        printf("\n");
    }

    if (args.show_undef) {
        printf("  Undefined symbols (iundefsym: %d, nundefsym:%d)\n", dysymtabCmd->iundefsym, dysymtabCmd->nundefsym);
        printSymbols(base, symtabCmd, dysymtabCmd->iundefsym, dysymtabCmd->nundefsym);
        printf("\n");
    }

    if (args.show_indirect) {
        printf("  Indirect symbol table (indirectsymoff: 0x%x, nindirectsyms: %d)\n", dysymtabCmd->indirectsymoff, dysymtabCmd->nindirectsyms);
        uint32_t *indirectSymtab = (uint32_t *)(base + dysymtabCmd->indirectsymoff); // the index is 32 bits
        printIndirectSymbols(base, symtabCmd, indirectSymtab, dysymtabCmd->nindirectsyms);
    }
}

static bool symtabLoadCommand(struct load_command *lcmd) {
    return lcmd->cmd == LC_SYMTAB;
}

static void printSymbols(uint8_t *base, struct symtab_command *symtabCmd, int offset, int num) {
    int max_number = args.no_truncate ? num : std::min(num, 10);
    for (int i = 0; i < max_number; ++i) {
        printSymbol(4, base, symtabCmd, offset + i);
    }

    if (!args.no_truncate && num > 10) {
        printf("        ... %d more ...\n", num - 10);
    }
}

static void printIndirectSymbols(uint8_t *base, struct symtab_command *symtabCmd, uint32_t *indirectSymtab, int size) {
    int max_number = args.no_truncate ? size : std::min(size, 10);
    for (int i = 0; i < max_number; ++i) {
        int index = *(indirectSymtab + i);
        const char *symbol = NULL;

        // Handle special index values
        if (index == INDIRECT_SYMBOL_LOCAL) {
            symbol = "INDIRECT_SYMBOL_LOCAL";
        } else if (index == INDIRECT_SYMBOL_ABS) {
            symbol = "INDIRECT_SYMBOL_ABS";
        } else if (index == (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) {
            symbol = "INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS";
        }

        if (symbol != NULL) {
            printf("    %-2d -> %s\n", i, symbol);
        } else {
            printf("    %-2d -> ", i);
            if (index >= 0 && index < symtabCmd->nsyms) {
                printSymbol(0, base, symtabCmd, index);
            } else {
                printf("%d (The index is out of bounds of symtab.)\n", index);
            }
        }
    }

    if (!args.no_truncate && size > 10) {
        printf("        ... %d more ...\n", size - 10);
    }
}
