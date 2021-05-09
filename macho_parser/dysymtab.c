#include <stdlib.h>
#include <mach-o/nlist.h>
#include "argument.h"
#include "dysymtab.h"

extern void *load_bytes(FILE *fptr, int offset, int size);

void symtab_cmd(FILE *fptr, void **sym_table, void **str_table);

void parse_dynamic_symbol_table(FILE *fptr, struct dysymtab_command *dysym_cmd) {
    printf("%-20s cmdsize: %-6u nlocalsym: %d  nextdefsym: %d   nundefsym: %d   nindirectsyms: %d \n",
        "LC_DYSYMTAB", dysym_cmd->cmdsize, dysym_cmd->nlocalsym, dysym_cmd->nextdefsym,  dysym_cmd->nundefsym, dysym_cmd->nindirectsyms);

    if (args.short_desc) { return; }

    void *sym_table = NULL;
    void *str_table = NULL;

    symtab_cmd(fptr, &sym_table, &str_table);

    printf("    Indirect symbol table (indirectsymoff: 0x%x, nindirectsyms: %d)\n", dysym_cmd->indirectsymoff, dysym_cmd->nindirectsyms);
    uint32_t *indirect_symtab = (uint32_t *)load_bytes(fptr, dysym_cmd->indirectsymoff, dysym_cmd->nindirectsyms * sizeof(uint32_t)); // the index is 32 bits

    char *symbol =NULL;
    for (int i = 0; i < dysym_cmd->nindirectsyms; ++i) {
        int index = *(indirect_symtab + i);
        if (index == INDIRECT_SYMBOL_LOCAL) {
            symbol = "INDIRECT_SYMBOL_LOCAL";
        } else if (index == INDIRECT_SYMBOL_ABS) {
            symbol = "INDIRECT_SYMBOL_ABS";
        } else {
            struct nlist_64 *nlist = sym_table + sizeof(struct nlist_64) * index;
            symbol = str_table + nlist->n_un.n_strx;
        }

        printf("        %d -> %s\n", index, symbol);
    }

    free(sym_table);
    free(str_table);
}


void symtab_cmd(FILE *fptr, void **sym_table, void **str_table) {
    struct mach_header_64 *header = load_bytes(fptr, 0, sizeof(struct mach_header_64));
    int offset = sizeof(struct mach_header_64);
    for (int i = 0; i < header->ncmds; ++i) {
        struct load_command *lcmd = load_bytes(fptr, offset, sizeof(struct load_command));

        if (lcmd->cmd == LC_SYMTAB) {
            struct symtab_command *symtab_cmd = load_bytes(fptr, offset, lcmd->cmdsize);
            *sym_table = load_bytes(fptr, symtab_cmd->symoff, symtab_cmd->nsyms * sizeof(struct nlist_64));
            *str_table = load_bytes(fptr, symtab_cmd->stroff, symtab_cmd->strsize);
            free(lcmd);
            return;
        }

        offset += lcmd->cmdsize;
        free(lcmd);
    }
}

