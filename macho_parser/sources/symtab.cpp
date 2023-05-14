#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>
#include <string>
#include <vector>
#include <sstream>
#include <iterator>

#include "argument.h"
#include "macho_binary.h"

#include "symtab.h"

static std::string formatSymbol(struct nlist_64 *nlist, uint8_t *strTable);
static std::string stringifyType(uint8_t type);
static std::string stringifyDescription(struct nlist_64 *nlist);
static std::string stringifyStabType(uint8_t type);

void printSymbolTable(uint8_t *base, struct symtab_command *symtabCmd) {
    printf("%-20s cmdsize: %-6d symoff: %d   nsyms: %d   (symsize: %lu)   stroff: %d   strsize: %u\n",
        "LC_SYMTAB", symtabCmd->cmdsize, symtabCmd->symoff, symtabCmd->nsyms,
        symtabCmd->nsyms * sizeof(struct nlist_64), symtabCmd->stroff, symtabCmd->strsize);

    if (args.verbosity == 0) { return; }

    void *symTable = base + symtabCmd->symoff;
    void *strTable = base + symtabCmd->stroff;

    for (int i = 0; i < symtabCmd->nsyms; ++i) {
        printSymbol(2, base, symtabCmd, i);
    }
}

void printSymbol(int indent, uint8_t *base, struct symtab_command *symtabCmd, int index) {
    if (index < 0 || index >= symtabCmd->nsyms) {
        puts("Error: %d is out of bounds of symtab.");
        exit(0);
    }

    uint8_t *symTable = base + symtabCmd->symoff;
    uint8_t *strTable = base + symtabCmd->stroff;

    struct nlist_64 *nlist = (struct nlist_64 *)(symTable + sizeof(struct nlist_64) * index);

    char formatted_value[32] = {'\0'};
    if ((nlist->n_type & N_TYPE) != N_UNDF) {
        snprintf(formatted_value, sizeof(formatted_value), "%016llx", nlist->n_value);
    }

    printf("%*s%-4d: %16s  %-10s  %-60s  %s\n",
        indent, "", index, formatted_value,
        stringifyType(nlist->n_type).c_str(),
        formatSymbol(nlist, strTable).c_str(),
        stringifyDescription(nlist).c_str());
}

static std::string formatSymbol(struct nlist_64 *nlist, uint8_t *strTable) {
    char buf[1024];
    char *symbol = (char *)(strTable + nlist->n_un.n_strx);
    if (nlist->n_type & N_STAB) {
        snprintf(buf, sizeof(buf), "%04d %5s %s \033[0;34m\033[0m", nlist->n_desc, stringifyStabType(nlist->n_type).c_str(), symbol);
    } else {
        snprintf(buf, sizeof(buf), "\033[0;34m%s\033[0m", symbol);
    }

    return std::string(buf);
}

static std::string stringifyType(uint8_t type) {
    std::vector<std::string> attrs;

    if (type & N_STAB) {
        // If any of the N_STAB is set, the whole type becomes a stab type which is defined in stab.h
        attrs.push_back("STAB");
    } else {
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
    }

    std::ostringstream formattedStream;
    std::copy(attrs.begin(), attrs.end(), std::ostream_iterator<std::string>(formattedStream, " "));
    std::string formatted = formattedStream.str();
    formatted.pop_back(); // remove the last space
    return std::string("[") + formatted + "]";
}

static std::string stringifyDescription(struct nlist_64 *nlist) {
    uint8_t type = nlist->n_type;
    uint16_t desc = nlist->n_desc;

    std::vector<std::string> attrs;

    if (nlist->n_sect != NO_SECT) {
        if (nlist->n_sect <= MAX_SECT) {
            attrs.push_back(machoBinary.getSectionNameByOrdinal(nlist->n_sect));
        }
    }

    if ((type & N_STAB) == 0) { // This is not a stab symbol
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

            int libraryOrdinal = GET_LIBRARY_ORDINAL(desc);
            if (libraryOrdinal > 0) {
                attrs.push_back(std::string("from ") + machoBinary.getDylibNameByOrdinal(libraryOrdinal));
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

static std::string stringifyStabType(uint8_t type) {
    switch (type) {
        case N_GSYM:    return "GSYM";
        case N_FNAME:   return "FNAME";
        case N_FUN:     return "FUN";
        case N_STSYM:   return "STSYM";
        case N_LCSYM:   return "LCSYM";
        case N_BNSYM:   return "BNSYM";
        case N_AST:     return "AST";
        case N_OPT:     return "OPT";
        case N_RSYM:    return "RSYM";
        case N_SLINE:   return "SLINE";
        case N_ENSYM:   return "ENSYM";
        case N_SSYM:    return "SSYM";
        case N_SO:      return "SO";
        case N_OSO:     return "OSO";
        case N_LSYM:    return "LSYM";
        case N_BINCL:   return "BINCL";
        case N_SOL:     return "SOL";
        case N_PARAMS:  return "PARAMS";
        case N_OLEVEL:  return "OLEVEL";
        case N_PSYM:    return "PSYM";
        case N_EINCL:   return "EINCL";
        case N_ENTRY:   return "ENTRY";
        case N_LBRAC:   return "LBRAC";
        case N_EXCL:    return "EXCL";
        case N_RBRAC:   return "RBRAC";
        case N_BCOMM:   return "BCOMM";
        case N_ECOMM:   return "ECOMM";
        case N_ECOML:   return "ECOML";
        case N_LENG:    return "LENG";
    }

    return std::to_string(type);
}

char *lookup_symbol_by_address(uint64_t address, uint8_t *base, struct symtab_command *symtabCmd) {
    uint8_t *symTable = base + symtabCmd->symoff;
    uint8_t *strTable = base + symtabCmd->stroff;

    // This logic can be optimized if the symbols are sorted by its address.
    for (int i = 0; i < symtabCmd->nsyms; ++i) {
        struct nlist_64 *nlist = (struct nlist_64 *)(symTable + sizeof(struct nlist_64) * i);
        if (nlist->n_value == address) {
            char *symbol = (char *)(strTable + nlist->n_un.n_strx);
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
