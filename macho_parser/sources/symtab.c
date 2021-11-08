#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <mach-o/nlist.h>
#include "argument.h"
#include "symtab.h"

static void format_n_type(uint8_t n_type, char *formatted);
static void format_n_desc(uint8_t n_type, uint16_t n_desc, char *formatted);

void parse_symbol_table(void *base, struct symtab_command *symtab_cmd) {
    printf("%-20s cmdsize: %-6d symoff: %d   nsyms: %d   (symsize: %lu)   stroff: %d   strsize: %u\n",
        "LC_SYMTAB", symtab_cmd->cmdsize, symtab_cmd->symoff, symtab_cmd->nsyms,
        symtab_cmd->nsyms * sizeof(struct nlist_64), symtab_cmd->stroff, symtab_cmd->strsize);

    if (args.verbosity == 0) { return; }

    void *sym_table = base + symtab_cmd->symoff;
    void *str_table = base + symtab_cmd->stroff;

    int nsyms = symtab_cmd->nsyms;
    int max_number = args.no_truncate ? nsyms : (nsyms > 10 ? 10 : nsyms);

    for (int i = 0; i < max_number; ++i) {
        print_symbol(2, base, symtab_cmd, i);
    }

    if (!args.no_truncate&& nsyms > 10) {
        printf("        ... %d more ...\n", nsyms - 10);
    }
}

void print_symbol(int indent, void *base, struct symtab_command *symtab_cmd, int index) {
    if (index < 0 || index >= symtab_cmd->nsyms) {
        puts("Error: %d is out of bounds of symtab.");
        exit(0);
    }

    void *sym_table = base + symtab_cmd->symoff;
    void *str_table = base + symtab_cmd->stroff;

    char formatted_n_type[256];
    char formatted_n_desc[256];

    struct nlist_64 *nlist = sym_table + sizeof(struct nlist_64) * index;
    char *symbol = str_table + nlist->n_un.n_strx;

    format_n_type(nlist->n_type, formatted_n_type);
    format_n_desc(nlist->n_type, nlist->n_desc, formatted_n_desc);

    char formatted_value[32] = {'\0'};
    if ((nlist->n_type & N_TYPE) != N_UNDF) {
        sprintf(formatted_value, "%016llx  ", nlist->n_value);
    }

    printf("%*s%-4d: %s%s  %s  %s\n",
        indent, "", index, formatted_value, formatted_n_type, symbol, formatted_n_desc);

}

static void format_n_type(uint8_t n_type, char *formatted) {
    strcpy(formatted, "");

    if (n_type & N_EXT) {
        // global symbols
        strcat(formatted, " N_EXT");
    }
    if (n_type & N_PEXT) {
        // private external symbols
        strcat(formatted, " N_PEXT");
    }
    if (n_type & N_STAB) {
        // debugging symbols
        char stab[16];
        sprintf(stab, " N_STAB(%#02x)", n_type & N_STAB);
        strcat(formatted, stab);
    }

    switch (n_type & N_TYPE) {
        case N_UNDF:
            strcat(formatted, " N_UNDF");
            break;
        case N_ABS:
            strcat(formatted, " N_ABS ");
            break;
        case N_SECT:
            strcat(formatted, " N_SECT");
            break;
        case N_PBUD:
            strcat(formatted, " N_PBUD");
            break;
        case N_INDR:
            strcat(formatted, " N_INDR");
            break;
    }
    strcat(formatted, "]");

    formatted[0] = '[';
}


static void format_n_desc(uint8_t n_type, uint16_t n_desc, char *formatted) {
    strcpy(formatted, "");

    if ((n_type & N_TYPE) == N_UNDF) {
        switch (n_desc & REFERENCE_TYPE) {
            case REFERENCE_FLAG_UNDEFINED_NON_LAZY:
                strcat(formatted, " UNDEFINED_NON_LAZY");
                break;
            case REFERENCE_FLAG_UNDEFINED_LAZY:
                strcat(formatted, " UNDEFINED_LAZY");
                break;
            case REFERENCE_FLAG_DEFINED:
                strcat(formatted, " DEFINED");
                break;
            case REFERENCE_FLAG_PRIVATE_DEFINED:
                strcat(formatted, " PRIVATE_DEFINED");
                break;
            case REFERENCE_FLAG_PRIVATE_UNDEFINED_NON_LAZY:
                strcat(formatted, " PRIVATE_UNDEFINED_NON_LAZY");
                break;
            case REFERENCE_FLAG_PRIVATE_UNDEFINED_LAZY:
                strcat(formatted, " PRIVATE_UNDEFINED_LAZY");
                break;
        }
    }

    if (n_desc & REFERENCED_DYNAMICALLY) {
        strcat(formatted, " REFERENCED_DYNAMICALLY");
    }

    if (n_desc & N_NO_DEAD_STRIP) {
        strcat(formatted, " N_NO_DEAD_STRIP");
    }
    if (n_desc & N_WEAK_REF) {
        strcat(formatted, " N_WEAK_REF");
    }
    if (n_desc & N_WEAK_DEF) {
        strcat(formatted, " N_WEAK_DEF");
    }

    int library_ordinal = GET_LIBRARY_ORDINAL(n_desc);
    if (library_ordinal > 0) {
        sprintf(formatted + strlen(formatted), " LIBRARY_ORDINAL(%d)", library_ordinal);
    }

    if (strlen(formatted) > 0) {
        strcat(formatted, "]");
        formatted[0] = '[';
    }
}
