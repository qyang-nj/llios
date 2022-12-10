#include <string.h>
#include <stdio.h>

#include "argument.h"
#include "util.h"
#include "load_command.h"
#include "symtab.h"

#include "segment_64.h"

static void print_section(void *base, struct section_64 sect, int section_index);
static void print_cstring_section(void *base, struct section_64 *sect);
static void print_pointer_section(void *base, struct section_64 *sect);
static void format_section_type(uint8_t type, char *out);
static bool has_section_to_show(int section_index, int count);

void parse_segment(void *base, struct segment_command_64 *seg_cmd, int section_index) {
    if (args.section_count > 0 && !has_section_to_show(section_index, seg_cmd->nsects)) {
        // If --section is specificied and no section needs to be show in this segment, just return.
        return;
    }

    char formatted_filesize[16];
    char formatted_vmsize[16];

    format_size(seg_cmd->filesize, formatted_filesize);
    format_size(seg_cmd->vmsize, formatted_vmsize);

    printf("%-20s cmdsize: %-6d segname: %-12.16s   file: 0x%08llx-0x%08llx %-9s  vm: 0x%09llx-0x%09llx %-9s prot: %d/%d\n",
        "LC_SEGMENT_64", seg_cmd->cmdsize, seg_cmd->segname,
        seg_cmd->fileoff, seg_cmd->fileoff + seg_cmd->filesize, formatted_filesize,
        seg_cmd->vmaddr, seg_cmd->vmaddr + seg_cmd->vmsize, formatted_vmsize,
        seg_cmd->initprot, seg_cmd->maxprot);

    if (args.verbosity < 1) {
        return;
    }

    // section_64 is immediately after segment_command_64.
    struct section_64 *sections = (void *)seg_cmd + sizeof(struct segment_command_64);

    for (int i = 0; i < seg_cmd->nsects; ++i) {
        struct section_64 sect = sections[i];
        if (showSection(section_index + i)) {
            print_section(base, sect, section_index + i);
        }
    }
}

static bool has_section_to_show(int section_index, int count) {
    for (int i = 0; i < count; ++i) {
        if (showSection(section_index + i)) {
            return true;
        }
    }
    return false;
}

static void print_section(void *base, struct section_64 sect, int section_index) {
    char formatted_type[32];
    char formatted_seg_sec[64];
    char formatted_size[16];

    const uint8_t type = sect.flags & SECTION_TYPE;

    format_section_type(type, formatted_type);
    sprintf(formatted_seg_sec, "(%.16s,%.16s)", sect.segname, sect.sectname);
    format_size(sect.size, formatted_size);

    printf("  %2d: 0x%09llx-0x%09llx %-11s %-32s  type: %s  offset: %d",
        section_index, sect.addr, sect.addr + sect.size, formatted_size, formatted_seg_sec, formatted_type, sect.offset);

    if (sect.reserved1 > 0) {
        printf("   reserved1: %2d", sect.reserved1);
    }

    if (sect.reserved2 > 0) {
        printf("   reserved1: %2d", sect.reserved2);
    }

    printf("\n");

    if (args.verbosity < 2) {
        return;
    }

    // (__TEXT,__cstring), (__TEXT,__objc_classname__TEXT), (__TEXT,__objc_methname), etc..
    if (type == S_CSTRING_LITERALS) {
        print_cstring_section(base, &sect);
    }
    // (__DATA_CONST,__mod_init_func)
    else if (type == S_MOD_INIT_FUNC_POINTERS
        || type == S_NON_LAZY_SYMBOL_POINTERS
        || type == S_LAZY_SYMBOL_POINTERS) {
        print_pointer_section(base, &sect);
    }
}

static void print_cstring_section(void *base, struct section_64 *sect) {
    void *section = base + sect->offset;
    char *formatted = malloc(1024 * 10);

    int count = 0;
    char *ptr = section;
    while(ptr < (char *)(section + sect->size)) {
        if (strlen(ptr) > 0) {
            format_string(ptr, formatted);
            printf("    \"%s\"\n", formatted);
            ptr += strlen(ptr);

            if (count >= 10 && !args.no_truncate) {
                break;
            }
            count += 1;
        }
        ptr += 1;
    }

    free(formatted);

    if (!args.no_truncate && ptr < (char *)(section + sect->size)) {
        printf("    ... more ...\n");
    }
}

static void print_pointer_section(void *base, struct section_64 *sect) {
    void *section = base + sect->offset;

    const size_t count = sect->size / sizeof(uintptr_t);
    int max_count = args.no_truncate ? count : MIN(count, 10);

    struct symtab_command *symtab_cmd = (struct symtab_command *)search_load_command(base, 0, is_symtab_load_command).lcmd;

    for (int i = 0; i < max_count; ++i) {
        char *symbol = lookup_symbol_by_address(*((uintptr_t *)section + i), base, symtab_cmd);
        printf("    0x%lx  %s\n", *((uintptr_t *)section + i), (symbol == NULL ? "" : symbol));
    }

    if (!args.no_truncate && count > 10) {
        printf("    ... %lu more ...\n", count - 10);
    }
}

static void format_section_type(uint8_t type, char *out) {
    if (type == S_REGULAR) {
        strcpy(out, "S_REGULAR");
    } else if (type == S_ZEROFILL) {
        strcpy(out, "S_ZEROFILL");
    } else if (type == S_CSTRING_LITERALS) {
        strcpy(out, "S_CSTRING_LITERALS");
    } else if (type == S_4BYTE_LITERALS) {
        strcpy(out, "S_4BYTE_LITERALS");
    } else if (type == S_8BYTE_LITERALS) {
        strcpy(out, "S_8BYTE_LITERALS");
    } else if (type == S_16BYTE_LITERALS) {
        strcpy(out, "S_16BYTE_LITERALS");
    } else if (type == S_LITERAL_POINTERS) {
        strcpy(out, "S_LITERAL_POINTERS");
    } else if (type == S_NON_LAZY_SYMBOL_POINTERS) {
        strcpy(out, "S_NON_LAZY_SYMBOL_POINTERS");
    } else if (type == S_LAZY_SYMBOL_POINTERS) {
        strcpy(out, "S_LAZY_SYMBOL_POINTERS");
    } else if (type == S_LITERAL_POINTERS) {
        strcpy(out, "S_LITERAL_POINTERS");
    } else if (type == S_SYMBOL_STUBS) {
        strcpy(out, "S_SYMBOL_STUBS");
    } else if (type == S_MOD_INIT_FUNC_POINTERS) {
        strcpy(out, "S_MOD_INIT_FUNC_POINTERS");
    } else if (type == S_THREAD_LOCAL_ZEROFILL) {
        strcpy(out, "S_THREAD_LOCAL_ZEROFILL");
    } else if (type == S_THREAD_LOCAL_VARIABLES) {
        strcpy(out, "S_THREAD_LOCAL_VARIABLES");
    } else if (type == S_COALESCED) {
        strcpy(out, "S_COALESCED");
    } else {
        sprintf(out, "OTHER(0x%x)", type);
    }
}
