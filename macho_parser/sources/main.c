#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include "argument.h"
#include "util.h"
#include "segment_64.h"
#include "symtab.h"
#include "dysymtab.h"
#include "dyld_info.h"
#include "linkedit_data.h"
#include "build_version.h"

void parse_load_commands(FILE *, int offset, uint32_t);
void parse_dylinker(FILE *, struct dylinker_command *);
void parse_entry_point(FILE *, struct entry_point_command *);
void parse_linker_option(FILE *, struct linker_option_command *);
void parse_dylib(FILE *, struct dylib_command *);
void parse_rpath(FILE *, struct rpath_command *);

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

        switch (lcmd->cmd) {
            case LC_SEGMENT_64:
                parse_segment(fptr, (struct segment_command_64 *)cmd);
                break;
            case LC_SYMTAB:
                parse_symbol_table(fptr, (struct symtab_command *)cmd);
                break;
            case LC_DYSYMTAB:
                parse_dynamic_symbol_table(fptr, (struct dysymtab_command *)cmd);
                break;
            case LC_LOAD_DYLINKER:
                parse_dylinker(fptr, (struct dylinker_command *)cmd);
                break;
            case LC_MAIN:
                parse_entry_point(fptr, (struct entry_point_command *)cmd);
                break;
            case LC_LINKER_OPTION:
                parse_linker_option(fptr, (struct linker_option_command *)cmd);
                break;
            case LC_ID_DYLIB:
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
                parse_dylib(fptr, (struct dylib_command *)cmd);
                break;
            case LC_RPATH:
                parse_rpath(fptr, (struct rpath_command *)cmd);
                break;
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY:
                parse_dyld_info(fptr, (struct dyld_info_command *)cmd);
                break;
            case LC_CODE_SIGNATURE:
            case LC_FUNCTION_STARTS:
            case LC_DATA_IN_CODE:
            case LC_DYLIB_CODE_SIGN_DRS:
            case LC_LINKER_OPTIMIZATION_HINT:
                parse_linkedit_data(fptr, (struct linkedit_data_command *)cmd);
                break;
            case LC_BUILD_VERSION:
                parse_build_version(fptr, (struct build_version_command *)cmd);
                break;
            case LC_VERSION_MIN_MACOSX:
            case LC_VERSION_MIN_IPHONEOS:
            case LC_VERSION_MIN_WATCHOS:
            case LC_VERSION_MIN_TVOS:
                parse_version_min(fptr, (struct version_min_command *)cmd);
                break;
            default:
                printf("LC_(%x)\n", lcmd->cmd);
        }

        offset += lcmd->cmdsize;
        free(cmd);
        free(lcmd);
    }
}

void parse_dylinker(FILE *fptr, struct dylinker_command *dylinker_cmd) {
    printf("%-20s cmdsize: %-6u %s\n", "LC_LOAD_DYLINKER",
        dylinker_cmd->cmdsize, (char *)dylinker_cmd + dylinker_cmd->name.offset);
}

void parse_entry_point(FILE *fptr, struct entry_point_command *entry_point_cmd) {
    uint64_t entryoff = entry_point_cmd->entryoff;
    printf("%-20s cmdsize: %-6u entryoff: 0x%llx(%llu) stacksize: %llu\n", "LC_MAIN",
        entry_point_cmd->cmdsize, entryoff, entryoff, entry_point_cmd->stacksize);
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



