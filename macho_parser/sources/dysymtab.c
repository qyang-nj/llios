#include <stdlib.h>
#include <mach-o/nlist.h>

#include "util.h"
#include "argument.h"
#include "symtab.h"
#include "dysymtab.h"

#define MIN(a,b) ((a) < (b) ? (a) : (b))

static void load_symtab_cmd(void *base, struct symtab_command **symtab_cmd);
static void print_symbols(void *base, struct symtab_command *symtab_cmd, int offset, int num);
static void print_indirect_symbols(void *base, struct symtab_command *symtab_cmd, uint32_t *indirect_symtab, int size);

void parse_dynamic_symbol_table(void *base, struct dysymtab_command *dysymtab_cmd) {
    printf("%-20s cmdsize: %-6u nlocalsym: %d  nextdefsym: %d   nundefsym: %d   nindirectsyms: %d \n",
        "LC_DYSYMTAB", dysymtab_cmd->cmdsize, dysymtab_cmd->nlocalsym,
        dysymtab_cmd->nextdefsym,  dysymtab_cmd->nundefsym, dysymtab_cmd->nindirectsyms);

    if (args.verbosity == 0) { return; }

    printf("  ilocalsym     : %-10d  nlocalsym    : %d\n", dysymtab_cmd->ilocalsym, dysymtab_cmd->nlocalsym);
    printf("  iextdefsym    : %-10d  nextdefsym   : %d\n", dysymtab_cmd->iextdefsym, dysymtab_cmd->nextdefsym);
    printf("  iundefsym     : %-10d  nundefsym    : %d\n", dysymtab_cmd->iundefsym, dysymtab_cmd->nundefsym);
    printf("  tocoff        : 0x%-8x  ntoc         : %d\n", dysymtab_cmd->tocoff, dysymtab_cmd->ntoc);
    printf("  modtaboff     : 0x%-8x  nmodtab      : %d\n", dysymtab_cmd->modtaboff, dysymtab_cmd->nmodtab);
    printf("  extrefsymoff  : 0x%-8x  nextrefsyms  : %d\n", dysymtab_cmd->extrefsymoff, dysymtab_cmd->nextrefsyms);
    printf("  indirectsymoff: 0x%08x  nindirectsyms: %d\n", dysymtab_cmd->indirectsymoff, dysymtab_cmd->nindirectsyms);
    printf("  extreloff     : 0x%-8x  nextrel      : %d\n", dysymtab_cmd->extreloff, dysymtab_cmd->nextrel);
    printf("  locreloff     : 0x%-8x  nlocrel      : %d\n", dysymtab_cmd->locreloff, dysymtab_cmd->nlocrel);
    printf("\n");

    struct symtab_command *symtab_cmd = (struct symtab_command *)get_load_command(base, LC_SYMTAB);

    if (args.show_local) {
        printf("  Local symbols (ilocalsym %d, nlocalsym:%d)\n", dysymtab_cmd->ilocalsym, dysymtab_cmd->nlocalsym);
        print_symbols(base, symtab_cmd, dysymtab_cmd->ilocalsym, dysymtab_cmd->nlocalsym);
        printf("\n");
    }

    if (args.show_extdef) {
        printf("  Externally defined symbols (iextdefsym: %d, nextdefsym:%d)\n", dysymtab_cmd->iextdefsym, dysymtab_cmd->nextdefsym);
        print_symbols(base, symtab_cmd, dysymtab_cmd->iextdefsym, dysymtab_cmd->nextdefsym);
        printf("\n");
    }

    if (args.show_undef) {
        printf("  Undefined symbols (iundefsym: %d, nundefsym:%d)\n", dysymtab_cmd->iundefsym, dysymtab_cmd->nundefsym);
        print_symbols(base, symtab_cmd, dysymtab_cmd->iundefsym, dysymtab_cmd->nundefsym);
        printf("\n");
    }

    if (args.show_indirect) {
        printf("  Indirect symbol table (indirectsymoff: 0x%x, nindirectsyms: %d)\n", dysymtab_cmd->indirectsymoff, dysymtab_cmd->nindirectsyms);
        uint32_t *indirect_symtab = base + dysymtab_cmd->indirectsymoff; // the index is 32 bits
        print_indirect_symbols(base, symtab_cmd, indirect_symtab, dysymtab_cmd->nindirectsyms);
    }
}


static void load_symtab_cmd(void *base, struct symtab_command **symtab_cmd) {
    struct mach_header_64 *header = base;
    int offset = sizeof(struct mach_header_64);
    for (int i = 0; i < header->ncmds; ++i) {
        struct load_command *lcmd = base + offset;

        if (lcmd->cmd == LC_SYMTAB) {
            *symtab_cmd = base + offset;
            return;
        }

        offset += lcmd->cmdsize;
    }
}

static void print_symbols(void *base, struct symtab_command *symtab_cmd, int offset, int num) {
    int max_number = args.no_truncate ? num : MIN(num, 10);
    for (int i = 0; i < max_number; ++i) {
        print_symbol(4, base, symtab_cmd, offset + i);
    }

    if (!args.no_truncate && num > 10) {
        printf("        ... %d more ...\n", num - 10);
    }
}

static void print_indirect_symbols(void *base, struct symtab_command *symtab_cmd, uint32_t *indirect_symtab, int size) {
    void *sym_table = base + symtab_cmd->symoff;
    void *str_table = base + symtab_cmd->stroff;

    int max_number = args.no_truncate ? size : MIN(size, 10);
    for (int i = 0; i < max_number; ++i) {
        int index = *(indirect_symtab + i);
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
            if (index >= 0 && index < symtab_cmd->nsyms) {
                print_symbol(0, base, symtab_cmd, index);
            } else {
                printf("%d (The index is out of bounds of symtab.)\n", index);
            }
        }
    }

    if (!args.no_truncate && size > 10) {
        printf("        ... %d more ...\n", size - 10);
    }
}
