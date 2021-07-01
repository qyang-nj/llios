#include <string.h>
#include <stdio.h>

#include "argument.h"
#include "util.h"

#include "segment_64.h"

static void parse_section(void *base, struct section_64 sect);
static void parse_cstring_section(void *base, struct section_64 *sect);
static void parse_pointer_section(void *base, struct section_64 *sect);
static void format_section_type(uint8_t type, char *out);

void parse_segment(void *base, struct segment_command_64 *seg_cmd) {
    char formatted_size1[16];
    char formatted_size2[16];

    sprintf(formatted_size1, "(%lld)", seg_cmd->filesize);
    sprintf(formatted_size2, "(%lld)", seg_cmd->vmsize);

    printf("%-20s cmdsize: %-6d segname: %-12s   file: 0x%08llx-0x%08llx %-11s  vm: 0x%09llx-0x%09llx %-12s protection: %d/%d\n",
        "LC_SEGMENT_64", seg_cmd->cmdsize, seg_cmd->segname,
        seg_cmd->fileoff, seg_cmd->fileoff + seg_cmd->filesize, formatted_size1,
        seg_cmd->vmaddr, seg_cmd->vmaddr + seg_cmd->vmsize, formatted_size2,
        seg_cmd->initprot, seg_cmd->maxprot);

    // section_64 is immediately after segment_command_64.
    struct section_64 *sections = (void *)seg_cmd + sizeof(struct segment_command_64);

    for (int i = 0; i < seg_cmd->nsects; ++i) {
        struct section_64 sect = sections[i];
        parse_section(base, sect);
    }
}

static void parse_section(void *base, struct section_64 sect) {
    char formatted_type[32];
    char formatted_seg_sec[64];
    char formatted_size[16];

    const uint8_t type = sect.flags & SECTION_TYPE;

    format_section_type(type, formatted_type);
    sprintf(formatted_seg_sec, "(%s,%s)", sect.segname, sect.sectname);
    sprintf(formatted_size, "(%lld)", sect.size);

    printf("    0x%09llx-0x%09llx %-11s %-32s  type: %s",
        sect.addr, sect.addr + sect.size, formatted_size, formatted_seg_sec, formatted_type);

    if (sect.reserved1 > 0) {
        printf("   reserved1: %2d", sect.reserved1);
    }

    if (sect.reserved2 > 0) {
        printf("   reserved1: %2d", sect.reserved2);
    }

    printf("\n");

    if (args.verbose == 0) {
        return;
    }

    // (__TEXT,__cstring), (__TEXT,__objc_classname__TEXT), (__TEXT,__objc_methname), etc..
    if (type == S_CSTRING_LITERALS) {
        parse_cstring_section(base, &sect);
    }
    // (__DATA_CONST,__mod_init_func)
    else if (type == S_MOD_INIT_FUNC_POINTERS
        || type == S_NON_LAZY_SYMBOL_POINTERS
        || type == S_LAZY_SYMBOL_POINTERS) {
        parse_pointer_section(base, &sect);
    }
}

static void parse_cstring_section(void *base, struct section_64 *sect) {
    void *section = base + sect->offset;

    char formatted[256];
    for (char *ptr = section; ptr < (char *)(section + sect->size);) {
        if (strlen(ptr) > 0) {
            format_string(ptr, formatted);
            printf("        \"%s\"\n", formatted);
            ptr += strlen(ptr);
        }
        ptr += 1;
    }
}

static void parse_pointer_section(void *base, struct section_64 *sect) {
    void *section = base + sect->offset;

    const size_t count = sect->size / sizeof(uintptr_t);
    for (int i = 0; i < count; ++i) {
        printf("        0x%lx\n", *((uintptr_t *)section + i));
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
    }else {
        sprintf(out, "OTHER(0x%x)", type);
    }
}
