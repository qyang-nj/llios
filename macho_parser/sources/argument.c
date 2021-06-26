#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "argument.h"

struct argument args;

void parse_arguments(int argc, char **argv) {
    int opt = 0;
    while((opt = getopt(argc, argv, "sc:v")) != -1) {
        switch(opt)
        {
            case 's':
                args.short_desc = true;
                break;
            case 'c':
                args.commands[args.command_count++] = string_to_load_command(optarg);
                break;
            case 'v':
                // increase verbose level if multiple -v is provided
                args.verbose++;
                break;
            case '?':
                fprintf(stderr, "Unknow option: %c.\n", optopt);
                exit(1);
        }
    }

    if (optind < argc) {
        args.file_name = argv[optind];
    }
}

unsigned int string_to_load_command(char *cmd_str) {
    if (strcmp(cmd_str, "LC_SEGMENT_64") == 0) {
        return LC_SEGMENT_64;
    } else if (strcmp(cmd_str, "LC_SYMTAB") == 0) {
        return LC_SYMTAB;
    } else if (strcmp(cmd_str, "LC_DYLD_INFO") == 0) {
        return LC_DYLD_INFO;
    } else if (strcmp(cmd_str, "LC_DYLD_INFO_ONLY") == 0) {
        return LC_DYLD_INFO_ONLY;
    } else if (strcmp(cmd_str, "LC_ID_DYLIB") == 0) {
        return LC_ID_DYLIB;
    } else if (strcmp(cmd_str, "LC_DYSYMTAB") == 0) {
        return LC_DYSYMTAB;
    } else if (strcmp(cmd_str, "LC_LOAD_DYLIB") == 0) {
        return LC_LOAD_DYLIB;
    } else if (strcmp(cmd_str, "LC_LOAD_WEAK_DYLIB") == 0) {
        return LC_LOAD_WEAK_DYLIB;
    } else if (strcmp(cmd_str, "LC_RPATH") == 0) {
        return LC_RPATH;
    } else if (strcmp(cmd_str, "LC_FUNCTION_STARTS") == 0) {
        return LC_FUNCTION_STARTS;
    } else if (strcmp(cmd_str, "LC_BUILD_VERSION") == 0) {
        return LC_BUILD_VERSION;
    } else if (strcmp(cmd_str, "LC_UUID") == 0) {
        return LC_UUID;
    } else if (strcmp(cmd_str, "LC_SOURCE_VERSION") == 0) {
        return LC_SOURCE_VERSION;
    } else if (strcmp(cmd_str, "LC_DYLD_CHAINED_FIXUPS") == 0) {
        return LC_DYLD_CHAINED_FIXUPS;
    }

    fprintf(stderr, "Unknow load command: %s.\n", cmd_str);
    exit(1);
}

bool show_command(unsigned int cmd) {
    if (args.command_count == 0) {
        // if no command is specified, show all commands.
        return true;
    }

    bool show = false;
    for (int i = 0; i < args.command_count; ++i) {
        if (args.commands[i] == cmd) {
            show = true;
            break;
        }
    }
    return show;
}
