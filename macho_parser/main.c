#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include "argument.h"
#include "symtab.h"
#include "dysymtab.h"
#include "dyld_info.h"

void parse_load_commands(FILE *, int offset, uint32_t);
void parse_segments(FILE *, struct segment_command_64 *);
void parse_cstring_section(FILE *, struct section_64 *);
void parse_pointer_section(FILE *, struct section_64 *);
void parse_dylinker(FILE *, struct dylinker_command *);
void parse_linker_option(FILE *, struct linker_option_command *);
void parse_dylib(FILE *, struct dylib_command *);
void parse_rpath(FILE *, struct rpath_command *);
void parse_linkedit_data(FILE *, struct linkedit_data_command *);

void format_section_type(uint8_t , char *);
void format_string(char *, char *);

void *load_bytes(FILE *fptr, int offset, int size) {
    void *buf = calloc(1, size);
    fseek(fptr, offset, SEEK_SET);
    fread(buf, size, 1, fptr);
    return buf;
}

int main(int argc, char **argv) {
    parse_arguments(argc, argv);

    if (args.file_name == NULL) {
        puts("Usage: parser [-s] [-c <cmd>] <mach-o file>");
        return 1;
    }

    FILE *fptr = fopen(args.file_name, "rb");
    if (fptr == NULL) {
        fprintf(stderr, "Cannot open file %s\n", args.file_name);
        return 1;
    }

    struct mach_header_64 *header = load_bytes(fptr, 0, sizeof(struct mach_header_64));
    parse_load_commands(fptr, sizeof(struct mach_header_64), header->ncmds);

    free(header);
    fclose(fptr);
    return 0;
}

void parse_load_commands(FILE *fptr, int offset, uint32_t ncmds) {
    for (int i = 0; i < ncmds; ++i) {
        struct load_command *lcmd = load_bytes(fptr, offset, sizeof(struct load_command));

        if (!show_command(lcmd->cmd)) {
            offset += lcmd->cmdsize;
            free(lcmd);
            continue;
        }

        void *cmd = load_bytes(fptr, offset, lcmd->cmdsize);

        if (lcmd->cmd == LC_SEGMENT_64) {
            parse_segments(fptr, (struct segment_command_64 *)cmd);
        } else if (lcmd->cmd == LC_SYMTAB) {
            parse_symbol_table(fptr, (struct symtab_command *)cmd);
        } else if (lcmd->cmd == LC_DYSYMTAB) {
            parse_dynamic_symbol_table(fptr, (struct dysymtab_command *)cmd);
        } else if (lcmd->cmd == LC_LOAD_DYLINKER) {
            parse_dylinker(fptr, (struct dylinker_command *)cmd);
        } else if (lcmd->cmd == LC_LINKER_OPTION) {
            parse_linker_option(fptr, (struct linker_option_command *)cmd);
        } else if (lcmd->cmd == LC_ID_DYLIB
            || lcmd->cmd == LC_LOAD_DYLIB
            || lcmd->cmd == LC_LOAD_WEAK_DYLIB) {
            parse_dylib(fptr, (struct dylib_command *)cmd);
        } else if (lcmd->cmd == LC_RPATH) {
            parse_rpath(fptr, (struct rpath_command *)cmd);
        } else if (lcmd->cmd == LC_DYLD_INFO
            || lcmd->cmd == LC_DYLD_INFO_ONLY) {
            parse_dyld_info(fptr, (struct dyld_info_command *)cmd);
        } else if (lcmd->cmd == LC_CODE_SIGNATURE
            || lcmd->cmd == LC_SEGMENT_SPLIT_INFO
            || lcmd->cmd == LC_FUNCTION_STARTS
            || lcmd->cmd == LC_DATA_IN_CODE
            || lcmd->cmd == LC_DYLIB_CODE_SIGN_DRS
            || lcmd->cmd == LC_LINKER_OPTIMIZATION_HINT) {
            parse_linkedit_data(fptr, (struct linkedit_data_command *)cmd);
        } else {
            printf("LC_(%x)\n", lcmd->cmd);
        }

        offset += lcmd->cmdsize;
        free(cmd);
        free(lcmd);
    }
}

void parse_segments(FILE *fptr, struct segment_command_64 *seg_cmd) {
    printf("%-20s cmdsize: %-6d segname: %-16s fileoff: 0x%08llx  filesize: %-12lld (fileend: 0x%08llx)\n",
        "LC_SEGMENT_64", seg_cmd->cmdsize, seg_cmd->segname, seg_cmd->fileoff, seg_cmd->filesize,
        seg_cmd->fileoff + seg_cmd->filesize);

    if (args.short_desc) { return; }

    // section_64 is immediately after segment_command_64.
    struct section_64 *sections = (void *)seg_cmd + sizeof(struct segment_command_64);

    char formatted_type[32];
    char formatted_seg_sec[64];

    for (int i = 0; i < seg_cmd->nsects; ++i) {
        struct section_64 sect = sections[i];
        const uint8_t type = sect.flags & SECTION_TYPE;

        format_section_type(type, formatted_type);
        sprintf(formatted_seg_sec, "(%s,%s)", sect.segname, sect.sectname);
        printf("    %-32s [size: %4lld] [type: %-32s] [reserved1: %2d, reserved2: %2d]\n",
            formatted_seg_sec, sect.size, formatted_type, sect.reserved1, sect.reserved2);

        // (__TEXT,__cstring), (__TEXT,__objc_classname__TEXT), (__TEXT,__objc_methname), etc..
        if (type == S_CSTRING_LITERALS) {
            parse_cstring_section(fptr, &sect);
        }
        // (__DATA_CONST,__mod_init_func)
        else if (type == S_MOD_INIT_FUNC_POINTERS
            || type == S_NON_LAZY_SYMBOL_POINTERS
            || type == S_LAZY_SYMBOL_POINTERS) {
            parse_pointer_section(fptr, &sect);
        }
    }
}

void parse_cstring_section(FILE *fptr, struct section_64 *cstring_sect) {
    void *section = load_bytes(fptr, cstring_sect->offset, cstring_sect->size);

    char formatted[256];
    for (char *ptr = section; ptr < (char *)(section + cstring_sect->size);) {
        if (strlen(ptr) > 0) {
            format_string(ptr, formatted);
            printf("        \"%s\"\n", formatted);
            ptr += strlen(ptr);
        }
        ptr += 1;
    }

    free(section);
}

void parse_pointer_section(FILE *fptr, struct section_64 *sect) {
    void *section = load_bytes(fptr, sect->offset, sect->size);

    const size_t count = sect->size / sizeof(uintptr_t);
    for (int i = 0; i < count; ++i) {
        printf("        0x%lx\n", *((uintptr_t *)section + i));
    }

    free(section);
}

void parse_dylinker(FILE *fptr, struct dylinker_command *dylinker_cmd) {
    printf("%-20s cmdsize: %-6u %s\n", "LC_LOAD_DYLINKER",
        dylinker_cmd->cmdsize, (char *)dylinker_cmd + dylinker_cmd->name.offset);
}

void parse_linker_option(FILE *fptr, struct linker_option_command *cmd) {
    char *options = calloc(1, cmd->cmdsize);
    memcpy(options, (char *)cmd + sizeof(struct linker_option_command), cmd->cmdsize -  sizeof(struct linker_option_command));

    char *opt = options;
    // replace '\n' to ' '. For example "abc\0def\0" -> "abc def\0"
    for (int i = 0; i < cmd->count - 1; ++i) {
        int len = strlen(opt);
        options[strlen(opt)] = ' ';
        opt = opt + len;
    }

    printf("%-20s cmdsize: %-6u count: %d   %s\n", "LC_LINKER_OPTION", cmd->cmdsize, cmd->count, options);
    free(options);
}

void parse_dylib(FILE *fptr, struct dylib_command *cmd) {
    char *cmd_name = "";
    if (cmd->cmd == LC_ID_DYLIB) {
        cmd_name = "LC_ID_DYLIB";
    } else if (cmd->cmd == LC_LOAD_DYLIB) {
        cmd_name = "LC_LOAD_DYLIB";
    } else if (cmd->cmd == LC_LOAD_WEAK_DYLIB) {
        cmd_name = "LC_LOAD_WEAK_DYLIB";
    }
    printf("%-20s cmdsize: %-6u %s\n", cmd_name, cmd->cmdsize, (char *)cmd + cmd->dylib.name.offset);
}

void parse_rpath(FILE *fptr, struct rpath_command *cmd) {
    printf("%-20s cmdsize: %-6u %s\n", "LC_RPATH", cmd->cmdsize, (char *)cmd + cmd->path.offset);
}

void parse_linkedit_data(FILE *fptr, struct linkedit_data_command *linkedit_data_cmd) {
    char *cmd_name = "";
    if (linkedit_data_cmd->cmd == LC_CODE_SIGNATURE) {
        cmd_name = "LC_CODE_SIGNATURE";
    } else if (linkedit_data_cmd->cmd == LC_SEGMENT_SPLIT_INFO) {
        cmd_name = "LC_SEGMENT_SPLIT_INFO";
    } else if (linkedit_data_cmd->cmd == LC_FUNCTION_STARTS) {
        cmd_name = "LC_FUNCTION_STARTS";
    } else if (linkedit_data_cmd->cmd == LC_DATA_IN_CODE) {
        cmd_name = "LC_DATA_IN_CODE";
    } else if (linkedit_data_cmd->cmd == LC_DYLIB_CODE_SIGN_DRS) {
        cmd_name = "LC_DYLIB_CODE_SIGN_DRS";
    } else if (linkedit_data_cmd->cmd == LC_LINKER_OPTIMIZATION_HINT) {
        cmd_name = "LC_LINKER_OPTIMIZATION_HINT";
    }

    printf("%-20s cmdsize: %-6u dataoff: %d   datasize: %d\n",
        cmd_name, linkedit_data_cmd->cmdsize, linkedit_data_cmd->dataoff, linkedit_data_cmd->datasize);
}

void format_section_type(uint8_t type, char *out) {
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
    } else {
        sprintf(out, "OTHER(0x%x)", type);
    }
}

// If the string contains '\n', replace with literal "\n".s
void format_string(char *str, char *formatted) {
    int j = 0;
    for (int i = 0; str[i] != '\0'; ++i) {
        switch(str[i]) {
            case '\n':
                formatted[j++] = '\\';
                formatted[j++] = 'n';
                break;
            default:
                formatted[j++] = str[i];
                break;
        }
    }
    formatted[j] = '\0';
}

