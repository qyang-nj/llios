#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include "argument.h"

struct argument args;

static struct option longopts[] = {
    {"help", no_argument, NULL, 'h'},
    {"command", required_argument, NULL, 'c'},
    {"verbose", no_argument, NULL, 'v'},
    {"no-truncate", no_argument, &(args.no_truncate), 1},
    {"build-version", no_argument, &(args.show_build_version), 1},
    {"code-signature", no_argument, &(args.show_code_signature), 1},
    {"cs", no_argument, &(args.show_code_signature), 1},
    {"code-directory", no_argument, &(args.show_code_direcotry), 1},
    {"cd", no_argument, &(args.show_code_direcotry), 1},
    {"entitlement", no_argument, &(args.show_entitlement), 1},
    {"ent", no_argument, &(args.show_entitlement), 1},
    {"blob-wrapper", no_argument, &(args.show_blob_wrapper), 1},
    {"dysymtab", no_argument, &(args.show_dysymtab), 1},
    {"local", no_argument, &(args.show_local), 1},
    {"extdef", no_argument, &(args.show_extdef), 1},
    {"undef", no_argument, &(args.show_undef), 1},
    {"indirect", no_argument, &(args.show_indirect), 1},
    {NULL, 0, NULL, 0}
};

void usage() {
    puts("Usage: macho_parser [options] macho_file");
    puts("    -c, --command LOAD_COMMAND           show specific load command");
    puts("    -v, --verbose                        can be used multiple times to increase verbose level");
    puts("        --no-truncate                    do not truncate even the content is long");
    puts("    -h, --help                           show this help message");
    puts("");
    puts("    --build-version                      equivalent to '--comand LC_BUILD_VERSION --comand LC_VERSION_MIN_*");
    puts("");
    puts("Code Signature Options:");
    puts("    --cs,  --code-signature              equivalent to '--command LC_CODE_SIGNATURE'");
    puts("    --cd,  --code-directory              show Code Directory");
    puts("    --ent, --entitlement                 show the embedded entitlement");
    puts("           --blob-wrapper                show the blob wrapper (signature blob)");
    puts("");
    puts("Dynamic Symbol Table Options:");
    puts("    --dysymtab                           equivalent to '--command LC_DYSYMTAB'");
    puts("    --local                              show local symbols");
    puts("    --extdef                             show externally (public) defined symbols");
    puts("    --undef                              show undefined symbols");
    puts("    --indirect                           show indirect symbol table");
}

void parse_arguments(int argc, char **argv) {
    int opt = 0;
    while ((opt = getopt_long(argc, argv, "c:shv", longopts, NULL)) != -1) {
        switch(opt) {
            case 'c':
                args.commands[args.command_count++] = string_to_load_command(optarg);
                break;
            case 'v':
                args.verbosity++;
                break;
            case 'h':
                usage();
                exit(0);
            case 0:
                // all long options that don't have short options
                break;
            case '?':
                // unrecognized option
                exit(1);
        }
    }

    if (optind < argc) {
        args.file_name = argv[optind];
    } else {
        puts("Error: missing a macho file.");
        exit(1);
    }

    if (args.show_build_version) {
        args.commands[args.command_count++] = LC_BUILD_VERSION;
        args.commands[args.command_count++] = LC_VERSION_MIN_MACOSX;
        args.commands[args.command_count++] = LC_VERSION_MIN_IPHONEOS;
        args.commands[args.command_count++] = LC_VERSION_MIN_WATCHOS;
        args.commands[args.command_count++] = LC_VERSION_MIN_TVOS;
    }

    if (args.show_code_signature || args.show_code_direcotry || args.show_entitlement || args.show_blob_wrapper) {
        args.commands[args.command_count++] = LC_CODE_SIGNATURE;
    }

    if (args.show_dysymtab || args.show_local || args. show_extdef || args.show_undef || args.show_indirect) {
        args.commands[args.command_count++] = LC_DYSYMTAB;
    }

    if (args.command_count > 0) {
        // Increase the verbosity if one or more command is specified.
        args.verbosity += 1;
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
    } else if (strcmp(cmd_str, "LC_DYLD_EXPORTS_TRIE") == 0) {
        return LC_DYLD_EXPORTS_TRIE;
    } else if (strcmp(cmd_str, "LC_DYLD_CHAINED_FIXUPS") == 0) {
        return LC_DYLD_CHAINED_FIXUPS;
    } else if (strcmp(cmd_str, "LC_CODE_SIGNATURE") == 0) {
        return LC_CODE_SIGNATURE;
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

bool show_header() {
    return args.command_count == 0;
}
