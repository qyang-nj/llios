#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <mach-o/nlist.h>
#include <string>
#include <vector>
#include <sstream>
#include <iterator>
#include <cctype>
#include <locale>
#include "argument.h"

extern "C" {
#include "dylib.h"
}
#include "macho_binary.h"
#include "symtab.h"

static std::string stringifyType(uint8_t type);
static std::string stringifyDescription(uint8_t type, uint16_t desc);
static inline void rtrim(std::string &s);

void printSymbolTable(uint8_t *base, struct symtab_command *symtab_cmd) {
    printf("%-20s cmdsize: %-6d symoff: %d   nsyms: %d   (symsize: %lu)   stroff: %d   strsize: %u\n",
        "LC_SYMTAB", symtab_cmd->cmdsize, symtab_cmd->symoff, symtab_cmd->nsyms,
        symtab_cmd->nsyms * sizeof(struct nlist_64), symtab_cmd->stroff, symtab_cmd->strsize);

    if (args.verbosity == 0) { return; }

    void *sym_table = base + symtab_cmd->symoff;
    void *str_table = base + symtab_cmd->stroff;

    int nsyms = symtab_cmd->nsyms;
    int max_number = args.no_truncate ? nsyms : (nsyms > 10 ? 10 : nsyms);

    for (int i = 0; i < max_number; ++i) {
        printSymbol(2, base, symtab_cmd, i);
    }

    if (!args.no_truncate&& nsyms > 10) {
        printf("        ... %d more ...\n", nsyms - 10);
    }
}

void printSymbol(int indent, uint8_t *base, struct symtab_command *symtab_cmd, int index) {
    if (index < 0 || index >= symtab_cmd->nsyms) {
        puts("Error: %d is out of bounds of symtab.");
        exit(0);
    }

    uint8_t *sym_table = base + symtab_cmd->symoff;
    uint8_t *str_table = base + symtab_cmd->stroff;

    struct nlist_64 *nlist = (struct nlist_64 *)(sym_table + sizeof(struct nlist_64) * index);
    char *symbol = (char *)(str_table + nlist->n_un.n_strx);

    char formatted_value[32] = {'\0'};
    if ((nlist->n_type & N_TYPE) != N_UNDF) {
        sprintf(formatted_value, "%016llx  ", nlist->n_value);
    }

    printf("%*s%-4d: %18s%-12s  \033[0;34m%s\033[0m  %s\n",
        indent, "", index, formatted_value,
        stringifyType(nlist->n_type).c_str(),
        symbol,
        stringifyDescription(nlist->n_type, nlist->n_desc).c_str());
}

static std::string stringifyType(uint8_t type) {
    std::vector<std::string> attrs;

    if (type & N_STAB) {
        attrs.push_back("STAB"); // debug symbols
    }

    switch (type & N_TYPE) {
        case N_UNDF:
            attrs.push_back("UNDF");
            break;
        case N_ABS:
            attrs.push_back("ABS");
            break;
        case N_SECT:
            attrs.push_back("SECT");
            break;
        case N_PBUD:
            attrs.push_back("PBUD");
            break;
        case N_INDR:
            attrs.push_back("INDR");
            break;
    }

    if (type & N_EXT) {
        attrs.push_back("EXT"); // global symbols
    }

    if (type & N_PEXT) {
        attrs.push_back("PEXT"); // private external symbols
    }

    std::ostringstream formattedStream;
    std::copy(attrs.begin(), attrs.end(), std::ostream_iterator<std::string>(formattedStream, " "));
    std::string formatted = formattedStream.str();
    formatted.pop_back(); // remove the last space
    return std::string("[") + formatted + "]";
}

static std::string stringifyDescription(uint8_t type, uint16_t desc) {
    std::vector<std::string> attrs;

    if ((type & N_TYPE) == N_UNDF) {
        switch (desc & REFERENCE_TYPE) {
            case REFERENCE_FLAG_UNDEFINED_NON_LAZY:
                attrs.push_back("UNDEFINED_NON_LAZY");
                break;
            case REFERENCE_FLAG_UNDEFINED_LAZY:
                attrs.push_back("UNDEFINED_LAZY");
                break;
            case REFERENCE_FLAG_DEFINED:
                attrs.push_back("DEFINED");
                break;
            case REFERENCE_FLAG_PRIVATE_DEFINED:
                attrs.push_back("PRIVATE_DEFINED");
                break;
            case REFERENCE_FLAG_PRIVATE_UNDEFINED_NON_LAZY:
                attrs.push_back("PRIVATE_UNDEFINED_NON_LAZY");
                break;
            case REFERENCE_FLAG_PRIVATE_UNDEFINED_LAZY:
                attrs.push_back("PRIVATE_UNDEFINED_LAZY");
                break;
        }
    }

    if (desc & REFERENCED_DYNAMICALLY) {
        attrs.push_back("REFERENCED_DYNAMICALLY");
    }

    if (desc & N_NO_DEAD_STRIP) {
        attrs.push_back("NO_DEAD_STRIP");
    }

    if (desc & N_WEAK_REF) {
        attrs.push_back("WEAK_REF");
    }
    if (desc & N_WEAK_DEF) {
        attrs.push_back("WEAK_DEF");
    }

    int libraryOrdinal = GET_LIBRARY_ORDINAL(desc);
    if (libraryOrdinal > 0) {
        attrs.push_back(std::string("from ") + machoBinary.getDylibNameByOrdinal(libraryOrdinal));
    }

    if (attrs.size() == 0) {
        return "";
    }

    std::ostringstream formattedStream;
    std::copy(attrs.begin(), attrs.end(), std::ostream_iterator<std::string>(formattedStream, ", "));
    std::string formatted = formattedStream.str();
    formatted.pop_back(); // remove last to charaters ", "
    formatted.pop_back();
    return std::string("// ") + formatted;
}

char *lookup_symbol_by_address(uint64_t address, uint8_t *base, struct symtab_command *symtab_cmd) {
    uint8_t *sym_table = base + symtab_cmd->symoff;
    uint8_t *str_table = base + symtab_cmd->stroff;

    // This logic can be optimized if the symbols are sorted by its address.
    for (int i = 0; i < symtab_cmd->nsyms; ++i) {
        struct nlist_64 *nlist = (struct nlist_64 *)(sym_table + sizeof(struct nlist_64) * i);
        if (nlist->n_value == address) {
            char *symbol = (char *)(str_table + nlist->n_un.n_strx);
            if (strlen(symbol) > 0) {
                return symbol;
            }
        }
    }

    return NULL;
}

bool is_symtab_load_command(struct load_command *lcmd) {
    return lcmd->cmd == LC_SYMTAB;
}
