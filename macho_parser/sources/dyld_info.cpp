#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <string>

extern "C" {
#include "argument.h"
#include "util.h"
#include "dylib.h"
}

#include "macho_binary.h"
#include "dyld_info.h"

enum BindType {
    regular,
    weak,
    lazy,
};

static void printRebaseTable(uint8_t *base, uint32_t offset, uint32_t size);
static void printRebaseOpcodes(uint8_t *base, uint32_t offset, uint32_t size);
static void printBindingTable(uint8_t *base, uint32_t offset, uint32_t size, enum BindType bindType);
static void printBindingOpcodes(uint8_t *base, uint32_t offset, uint32_t size);
static void printExport(uint8_t *base, uint32_t exportOff, uint32_t exportSize);

static int8_t convertSignedImm(uint8_t imm);
static std::string getDylibName(int dylibOrdinal);
static std::string stringifyDylibSpecial(int dylibSpecial);
static std::string stringifySymbolFlagForOpcode(int flag);
static std::string stringifySymbolFlagForTable(int flag);
static std::string stringifyTypeImmForOpcode(int type);
static std::string stringifyTypeImmForTable(int type);

void printDyldInfo(uint8_t *base, struct dyld_info_command *dyldInfoCmd) {
    const char *name = (dyldInfoCmd->cmd == LC_DYLD_INFO_ONLY ? "LC_DYLD_INFO_ONLY" : "LC_DYLD_INFO");
    printf("%-20s cmdsize: %-6u export_size: %d\n", name, dyldInfoCmd->cmdsize, dyldInfoCmd->export_size);

    if (args.verbosity == 0) { return; }

    printf("  rebase_off   : %-10d   rebase_size   : %d\n", dyldInfoCmd->rebase_off, dyldInfoCmd->rebase_size);
    printf("  bind_off     : %-10d   bind_size     : %d\n", dyldInfoCmd->bind_off, dyldInfoCmd->bind_size);
    printf("  weak_bind_off: %-10d   weak_bind_size: %d\n", dyldInfoCmd->weak_bind_off, dyldInfoCmd->weak_bind_size);
    printf("  lazy_bind_off: %-10d   lazy_bind_size: %d\n", dyldInfoCmd->lazy_bind_off, dyldInfoCmd->lazy_bind_size);
    printf("  export_off   : %-10d   export_size   : %d\n", dyldInfoCmd->export_off, dyldInfoCmd->export_size);

    if (args.show_rebase) {
        if (args.show_opcode) {
            printf("\n  Rebase Opcodes:\n");
            printRebaseOpcodes(base, dyldInfoCmd->rebase_off, dyldInfoCmd->rebase_size);
        } else {
            printf("\n  Rebase Table:\n");
            printRebaseTable(base, dyldInfoCmd->rebase_off, dyldInfoCmd->rebase_size);
        }
    }

    if (args.show_bind) {
        if (args.show_opcode) {
            printf("\n  Binding Opcodes:\n");
            printBindingOpcodes(base, dyldInfoCmd->bind_off, dyldInfoCmd->bind_size);
        } else {
            printf("\n  Binding Table:\n");
            printBindingTable(base, dyldInfoCmd->bind_off, dyldInfoCmd->bind_size, regular);
        }
    }

    if (args.show_lazy_bind) {
        if (args.show_opcode) {
            printf("\n  Lazy Binding Opcodes:\n");
            printBindingOpcodes(base, dyldInfoCmd->lazy_bind_off, dyldInfoCmd->lazy_bind_size);
        } else {
            printf("\n  Lazy Binding Table:\n");
            printBindingTable(base, dyldInfoCmd->lazy_bind_off, dyldInfoCmd->lazy_bind_size, lazy);
        }
    }

    if (args.show_weak_bind) {
        if (args.show_opcode) {
            printf("\n  Weak Binding Opcodes:\n");
            printBindingOpcodes(base, dyldInfoCmd->weak_bind_off, dyldInfoCmd->weak_bind_size);
        } else {
            printf("\n  Weak Binding Table:\n");
            printBindingTable(base, dyldInfoCmd->weak_bind_off, dyldInfoCmd->weak_bind_size, weak);
        }
    }

    if (args.show_export) {
        printExport(base, dyldInfoCmd->export_off, dyldInfoCmd->export_size);
    }
}

static void printRebaseTable(uint8_t *base, uint32_t offset, uint32_t size) {
}

static void printRebaseOpcodes(uint8_t *base, uint32_t offset, uint32_t size) {
    uint8_t *rebase = base + offset;
    int i = 0;
    uint64_t uleb = 0;

    while (i < size) {
        uint8_t opcode = *(rebase + i) & REBASE_OPCODE_MASK;
        uint8_t imm = *(rebase + i) & REBASE_IMMEDIATE_MASK;
        printf ("0x%04X ", i);
        ++i;

        switch (opcode) {
            case REBASE_OPCODE_DONE:
                printf("REBASE_OPCODE_DONE\n");
                break;
            case REBASE_OPCODE_SET_TYPE_IMM:
                printf("REBASE_OPCODE_SET_TYPE_IMM (%d)\n", imm);
                break;
            case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: {
                struct segment_command_64 *segCmd = machoBinary.segmentCommands[imm];
                i += read_uleb128(rebase + i, &uleb);
                printf("REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB (%d, 0x%08llx) -- %s\n",
                     imm, uleb, segCmd->segname);
                break;
            }
            case REBASE_OPCODE_ADD_ADDR_ULEB:
                i += read_uleb128(rebase + i, &uleb);
                printf("REBASE_OPCODE_ADD_ADDR_ULEB (0x%08llx)\n", uleb);
                break;
            case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
                printf("REBASE_OPCODE_ADD_ADDR_IMM_SCALED (%d)\n", imm);
                break;
            case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
                printf("REBASE_OPCODE_DO_REBASE_IMM_TIMES (%d)\n", imm);
                break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
                i += read_uleb128(rebase + i, &uleb);
                printf("REBASE_OPCODE_DO_REBASE_ULEB_TIMES (%llu)\n", uleb);
                break;
            case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
                i += read_uleb128(rebase + i, &uleb);
                printf("REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB (0x%08llx)\n", uleb);
                break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: {
                uint64_t count, skip;
                i += read_uleb128(rebase + i, &count);
                i += read_uleb128(rebase + i, &skip);
                printf("REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB (count: %llu, skip: %llu)\n", uleb, skip);
                break;
            }
            default: {
                char errMsg[32];
                snprintf(errMsg, sizeof(errMsg), "Unknown Opcode (%#x)", opcode);
                throw std::runtime_error(errMsg);
            }
        }
    }
}

static void printBindingTable(uint8_t *base, uint32_t offset, uint32_t size, enum BindType bindType) {
    const uint8_t *bind = base + offset;
    const int ptrSize = sizeof(void *);
    int i = 0;

    int segmentOrdinal = 0;
    int segmentOffset = 0;
    int type = 0;
    int dylibOrdinal = 0;
    char *symbolName = NULL;
    int symbolFlag = 0;
    int addend = 0;

    uint64_t uleb = 0;
    int64_t sleb = 0;

    auto printTableRow = [&segmentOrdinal, &segmentOffset, &dylibOrdinal, &symbolName, &type, &symbolFlag, &addend, bindType]() {
        struct segment_command_64 *segCmd = machoBinary.segmentCommands[segmentOrdinal];
        uint64_t address = segCmd->vmaddr + segmentOffset;

        struct section_64 *sect = machoBinary.getSectionByAddress(address);
        char *sectName = NULL;
        if (sect != NULL) {
            sectName = sect->sectname;
        }

        std::string sectName2 = std::string(segCmd->segname) + "," + sectName;
        printf("%-24s  0x%llX  ", sectName2.c_str(), address);

        switch (bindType) {
            case regular:
                printf("%s  %-20s  addend(%d)  %s %s\n", stringifyTypeImmForTable(type).c_str(),
                    getDylibName(dylibOrdinal).c_str(), addend, symbolName,
                    stringifySymbolFlagForTable(symbolFlag).c_str());
                break;
            case lazy:
                printf("%-20s %s %s\n", getDylibName(dylibOrdinal).c_str(), symbolName,
                stringifySymbolFlagForTable(symbolFlag).c_str());
                break;
            case weak:
                printf("%s  addend(%d)  %s %s\n", stringifyTypeImmForTable(type).c_str(),
                    addend, symbolName, stringifySymbolFlagForTable(symbolFlag).c_str());
                break;
        }
    };


    while (i < size) {
        uint8_t opcode = *(bind + i) & BIND_OPCODE_MASK;
        uint8_t imm = *(bind + i) & BIND_IMMEDIATE_MASK;
        ++i;

        switch (opcode) {
            case BIND_OPCODE_DONE:
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                dylibOrdinal = imm;
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                i += read_uleb128(bind + i, &uleb);
                dylibOrdinal = uleb;
                break;
            case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                // dylib special is zero or negative
                dylibOrdinal = convertSignedImm(imm);
                break;
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                symbolFlag = imm;
                symbolName = (char *)bind + i;
                i += strlen((char *)bind + i) + 1;
                break;
            case BIND_OPCODE_SET_TYPE_IMM:
                type = imm;
                break;
            case BIND_OPCODE_SET_ADDEND_SLEB:
                i += read_sleb128(bind + i, &sleb);
                addend = sleb;
                break;
            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                i += read_uleb128(bind + i, &uleb);
                segmentOrdinal = imm;
                segmentOffset = uleb;
                break;
            case BIND_OPCODE_ADD_ADDR_ULEB:
                i += read_uleb128(bind + i, &uleb);
                segmentOffset += uleb;
                break;
            case BIND_OPCODE_DO_BIND:
                printTableRow();
                segmentOffset += ptrSize;
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                printTableRow();
                i += read_uleb128(bind + i, &uleb);
                segmentOffset += uleb + ptrSize;
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                printTableRow();
                segmentOffset += imm * ptrSize + ptrSize;
                break;
            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
                uint64_t count, skip;
                i += read_uleb128(bind + i, &count);
                i += read_uleb128(bind + i, &skip);
                for (int j = 0; j < count; ++j) {
                    printTableRow();
                    segmentOffset += skip + ptrSize;
                }
                break;
            }
            case BIND_OPCODE_THREADED:
                throw std::runtime_error("Unhandled Opcode (BIND_OPCODE_THREADED)");
            default: {
                char errMsg[32];
                snprintf(errMsg, sizeof(errMsg), "Unknown Opcode (%#x)", opcode);
                throw std::runtime_error(errMsg);
            }
        }
    }
}

static void printBindingOpcodes(uint8_t *base, uint32_t offset, uint32_t size) {
    uint8_t *bind = base + offset;
    int i = 0;

    uint64_t uleb = 0;
    int64_t sleb = 0;

    while (i < size) {
        uint8_t opcode = *(bind + i) & BIND_OPCODE_MASK;
        uint8_t imm = *(bind + i) & BIND_IMMEDIATE_MASK;
        printf ("0x%04X ", i);
        ++i;

        switch (opcode) {
            case BIND_OPCODE_DONE:
                printf("BIND_OPCODE_DONE\n");
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                // dylib ordinal starts at 1
                printf("BIND_OPCODE_SET_DYLIB_ORDINAL_IMM (%d) -- %s\n",
                    imm, getDylibName(imm).c_str());
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                i += read_uleb128(bind + i, &uleb);
                printf("BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB (%llu) -- %s\n",
                    uleb, getDylibName(uleb).c_str());
                break;
            case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                printf("BIND_OPCODE_SET_DYLIB_SPECIAL_IMM (%s)\n",
                    stringifyDylibSpecial(convertSignedImm(imm)).c_str());
                break;
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                printf("BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM (%s, %s)\n",
                    stringifySymbolFlagForOpcode(imm).c_str(), (char *)bind + i);
                i += strlen((char *)bind + i) + 1;
                break;
            case BIND_OPCODE_SET_TYPE_IMM:
                printf("BIND_OPCODE_SET_TYPE_IMM (%s)\n", stringifyTypeImmForOpcode(imm).c_str());
                break;
            case BIND_OPCODE_SET_ADDEND_SLEB:
                i += read_sleb128(bind + i, &sleb);
                printf("BIND_OPCODE_SET_ADDEND_SLEB (%lld)\n", sleb);
                break;
            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: {
                struct segment_command_64 *segCmd = machoBinary.segmentCommands[imm];
                i += read_uleb128(bind + i, &uleb);
                printf("BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB (%d, 0x%08llx) -- %s\n",
                    imm, uleb, segCmd->segname);
                break;
            }
            case BIND_OPCODE_ADD_ADDR_ULEB:
                i += read_uleb128(bind + i, &uleb);
                printf("BIND_OPCODE_ADD_ADDR_ULEB (0x%08llx)\n", uleb);
                break;
            case BIND_OPCODE_DO_BIND:
                printf("BIND_OPCODE_DO_BIND ()\n");
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                i += read_uleb128(bind + i, &uleb);
                printf("BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB (0x%08llx)\n", uleb);
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                printf("BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED (%d)\n", imm);
                break;
            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
                uint64_t count, skip;
                i += read_uleb128(bind + i, &count);
                i += read_uleb128(bind + i, &skip);
                printf("BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB (count: %llu, skip: %llu)\n", uleb, skip);
                break;
            }
            case BIND_OPCODE_THREADED:
                throw std::runtime_error("Unhandled Opcode (BIND_OPCODE_THREADED)");
            default: {
                char errMsg[32];
                snprintf(errMsg, sizeof(errMsg), "Unknown Opcode (%#x)", opcode);
                throw std::runtime_error(errMsg);
            }
        }
    }
}

// Print out the export trie.
static void printExportTrie(uint8_t *exportStart, uint8_t *nodePtr, int level) {
    uint64_t terminalSize;
    int byteCount = read_uleb128(nodePtr, &terminalSize);
    uint8_t *childrenCountPtr = nodePtr + byteCount + terminalSize;

    if (terminalSize != 0) {
        printf(" (data: ");
        for (int i = 0; i < terminalSize; ++i) {
            printf("%02x", *(nodePtr + byteCount + i));
        }
        printf(")\n");
    } else {
        printf("\n");
    }

    // According to the source code in dyld,
    // the count number is not uleb128 encoded;
    uint8_t children_count = *childrenCountPtr;
    uint8_t *s = childrenCountPtr + 1;
    for (int i = 0; i < children_count; ++i) {
        printf("  %*s%s", level * 2, "", s);
        s += strlen((char *)s) + 1;

        uint64_t child_offset;
        byteCount = read_uleb128(s, &child_offset);
        s += byteCount; // now s points to the next child's edge string
        printExportTrie(exportStart, exportStart + child_offset, level + 1);
    }
}

static void printExport(uint8_t *base, uint32_t exportOff, uint32_t exportSize) {
    uint8_t *exportInfo = base + exportOff;
    printf ("\n  Exported Symbols (Trie):");
    printExportTrie(exportInfo, exportInfo, 0);
}

static int8_t convertSignedImm(uint8_t imm) {
    return (imm & 0x08) ? (imm | 0xF0) : imm;
}

static std::string stringifySymbolFlagForOpcode(int flag) {
    switch(flag) {
        case 0:
            return std::string("0");
        case BIND_SYMBOL_FLAGS_WEAK_IMPORT:
            return std::string("BIND_SYMBOL_FLAGS_WEAK_IMPORT");
        case BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION:
            return std::string("BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION");
    }
    return std::string("unknown(") + std::to_string(flag) + ")";
}

static std::string stringifySymbolFlagForTable(int flag) {
    char formatted[64];
    switch(flag) {
        case 0:
            sprintf(formatted, "%s", "");
            break;
        case BIND_SYMBOL_FLAGS_WEAK_IMPORT:
            sprintf(formatted, "(%s)", "weak import");
            break;
        case BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION:
            sprintf(formatted, "(%s)", "non weak definition");
            break;
        default:
            sprintf(formatted, "(%s)", "unknown");
            break;
    }
    return std::string(formatted);
}

static std::string stringifyTypeImmForOpcode(int type) {
    switch(type) {
        case BIND_TYPE_POINTER:
            return std::string("BIND_TYPE_POINTER");
        case BIND_TYPE_TEXT_ABSOLUTE32:
            return std::string("BIND_TYPE_TEXT_ABSOLUTE32");
        case BIND_TYPE_TEXT_PCREL32:
            return std::string("BIND_TYPE_TEXT_PCREL32");
    }
    return std::string("unknown(") + std::to_string(type) + ")";
}

static std::string stringifyTypeImmForTable(int type) {
    switch(type) {
    case BIND_TYPE_POINTER:
        return std::string("pointer");
    case BIND_TYPE_TEXT_ABSOLUTE32:
        return std::string("text_absolute32");
    case BIND_TYPE_TEXT_PCREL32:
        return std::string("text_pcrel32");
    }

    return std::string("unknown(") + std::to_string(type) + ")";
}

static std::string stringifyDylibSpecial(int dylibSpecial) {
    switch (dylibSpecial) {
    case BIND_SPECIAL_DYLIB_SELF:
        return std::string("BIND_SPECIAL_DYLIB_SELF");
    case BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE:
        // used by plug-in to link with host
        return std::string("BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE");
    case BIND_SPECIAL_DYLIB_FLAT_LOOKUP:
        return std::string("BIND_SPECIAL_DYLIB_FLAT_LOOKUP");
    case BIND_SPECIAL_DYLIB_WEAK_LOOKUP:
        return std::string("BIND_SPECIAL_DYLIB_WEAK_LOOKUP");
    }

    throw std::runtime_error(std::string("Invalid or unhandled dylib special: ") + std::to_string(dylibSpecial));
}

static std::string getDylibName(int dylibOrdinal) {
    switch (dylibOrdinal) {
    case BIND_SPECIAL_DYLIB_SELF:
        return std::string("self");
    case BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE:
        // used by plug-in to link with host
        return std::string("main-executable");
    case BIND_SPECIAL_DYLIB_FLAT_LOOKUP:
        return std::string("flat-namespace");
    case BIND_SPECIAL_DYLIB_WEAK_LOOKUP:
        return std::string("weak-lookup");
    }

    if (dylibOrdinal > 0) {
        struct dylib_command *dylibCmd = machoBinary.getDylibCommands()[dylibOrdinal - 1];
        return std::string(get_dylib_name(dylibCmd, true));
    }

    throw std::runtime_error(std::string("Invalid or unhandled dylib ordinal: ") + std::to_string(dylibOrdinal));
}
