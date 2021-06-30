#include <stdlib.h>
#include <mach-o/nlist.h>
#include "argument.h"
#include "dysymtab.h"

#define MIN(a,b) ((a) < (b) ? (a) : (b))

void load_symtab_cmd(void *base, void **sym_table, void **str_table);
void print_symbols(void *sym_table, void *str_table, int offset, int num);
void print_indirect_symbols(void *sym_table, void *str_table, uint32_t *indirect_symtab, int size);

void parse_dynamic_symbol_table(void *base, struct dysymtab_command *dysymtab_cmd) {
    printf("%-20s cmdsize: %-6u nlocalsym: %d  nextdefsym: %d   nundefsym: %d   nindirectsyms: %d \n",
        "LC_DYSYMTAB", dysymtab_cmd->cmdsize, dysymtab_cmd->nlocalsym,
        dysymtab_cmd->nextdefsym,  dysymtab_cmd->nundefsym, dysymtab_cmd->nindirectsyms);

    if (args.verbose == 0) { return; }

    printf("    ilocalsym     : %-10d  nlocalsym    : %d\n", dysymtab_cmd->ilocalsym, dysymtab_cmd->nlocalsym);
    printf("    iextdefsym    : %-10d  nextdefsym   : %d\n", dysymtab_cmd->iextdefsym, dysymtab_cmd->nextdefsym);
    printf("    iundefsym     : %-10d  nundefsym    : %d\n", dysymtab_cmd->iundefsym, dysymtab_cmd->nundefsym);
    printf("    tocoff        : 0x%-8x  ntoc         : %d\n", dysymtab_cmd->tocoff, dysymtab_cmd->ntoc);
    printf("    modtaboff     : 0x%-8x  nmodtab      : %d\n", dysymtab_cmd->modtaboff, dysymtab_cmd->nmodtab);
    printf("    extrefsymoff  : 0x%-8x  nextrefsyms  : %d\n", dysymtab_cmd->extrefsymoff, dysymtab_cmd->nextrefsyms);
    printf("    indirectsymoff: 0x%08x  nindirectsyms: %d\n", dysymtab_cmd->indirectsymoff, dysymtab_cmd->nindirectsyms);
    printf("    extreloff     : 0x%-8x  nextrel      : %d\n", dysymtab_cmd->extreloff, dysymtab_cmd->nextrel);
    printf("    locreloff     : 0x%-8x  nlocrel      : %d\n", dysymtab_cmd->locreloff, dysymtab_cmd->nlocrel);
    printf("\n");

    void *sym_table = NULL;
    void *str_table = NULL;

    load_symtab_cmd(base, &sym_table, &str_table);

    printf("    Local symbols (ilocalsym %d, nlocalsym:%d)\n", dysymtab_cmd->ilocalsym, dysymtab_cmd->nlocalsym);
    print_symbols(sym_table, str_table, dysymtab_cmd->ilocalsym, dysymtab_cmd->nlocalsym);
    printf("\n");

    printf("    Externally defined symbols (iextdefsym: %d, nextdefsym:%d)\n", dysymtab_cmd->iextdefsym, dysymtab_cmd->nextdefsym);
    print_symbols(sym_table, str_table, dysymtab_cmd->iextdefsym, dysymtab_cmd->nextdefsym);
    printf("\n");

    printf("    Undefined symbols (iundefsym: %d, nundefsym:%d)\n", dysymtab_cmd->iundefsym, dysymtab_cmd->nundefsym);
    print_symbols(sym_table, str_table, dysymtab_cmd->iundefsym, dysymtab_cmd->nundefsym);
    printf("\n");

    printf("    Indirect symbol table (indirectsymoff: 0x%x, nindirectsyms: %d)\n", dysymtab_cmd->indirectsymoff, dysymtab_cmd->nindirectsyms);
    uint32_t *indirect_symtab = base + dysymtab_cmd->indirectsymoff; // the index is 32 bits
    print_indirect_symbols(sym_table, str_table, indirect_symtab, dysymtab_cmd->nindirectsyms);
}


void load_symtab_cmd(void *base, void **sym_table, void **str_table) {
    struct mach_header_64 *header = base;
    int offset = sizeof(struct mach_header_64);
    for (int i = 0; i < header->ncmds; ++i) {
        struct load_command *lcmd = base + offset;

        if (lcmd->cmd == LC_SYMTAB) {
            struct symtab_command *symtab_cmd = base + offset; // load_bytes(fptr, offset, lcmd->cmdsize);
            *sym_table = base + symtab_cmd->symoff; //load_bytes(base, symtab_cmd->symoff, symtab_cmd->nsyms * sizeof(struct nlist_64));
            *str_table = base + symtab_cmd->stroff; //load_bytes(base, symtab_cmd->stroff, symtab_cmd->strsize);
            return;
        }

        offset += lcmd->cmdsize;
    }
}

void print_symbols(void *sym_table, void *str_table, int offset, int num) {
    int max_number = args.verbose == 1 ? MIN(num, 10) : num;
    for (int i = 0; i < max_number; ++i) {
        struct nlist_64 *nlist = sym_table + sizeof(struct nlist_64) * (offset + i);
        const char * symbol = str_table + nlist->n_un.n_strx;
        printf("        %s\n", symbol);
    }

    if (args.verbose == 1 && num > 10) {
        printf("        ... %d more ...\n", num - 10);
    }
}

void print_indirect_symbols(void *sym_table, void *str_table, uint32_t *indirect_symtab, int size) {
    int max_number = args.verbose == 1 ? MIN(size, 10) : size;
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
            printf("        %-2d: %s\n", i, symbol);
        } else {
            struct nlist_64 *nlist = sym_table + sizeof(struct nlist_64) * index;
            symbol = str_table + nlist->n_un.n_strx;
            printf("        %-2d: %-5d -> %s\n", i, index, symbol);
        }
    }

    if (args.verbose == 1 && size > 10) {
        printf("        ... %d more ...\n", size - 10);
    }
}
