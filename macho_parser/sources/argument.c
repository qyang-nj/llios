#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <assert.h>

#include "argument.h"

struct argument args;

static struct option longopts[] = {
    {"help", no_argument, NULL, 'h'},
    {"command", required_argument, NULL, 'c'},
    {"arch", required_argument, NULL, 0},
    {"verbose", no_argument, NULL, 'v'},
    {"no-truncate", no_argument, &(args.no_truncate), 1},
    {"segments", no_argument, &(args.show_segments), 1},
    {"section", required_argument, NULL, 's'},
    {"build-version", no_argument, &(args.show_build_version), 1},
    {"dylibs", no_argument, &(args.show_dylibs), 1},

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

    {"dyld-info", no_argument, &(args.show_dyld_info), 1},
    {"rebase", no_argument, &(args.show_rebase), 1},
    {"bind", no_argument, &(args.show_bind), 1},
    {"weak-bind", no_argument, &(args.show_weak_bind), 1},
    {"lazy-bind", no_argument, &(args.show_lazy_bind), 1},
    {"export", no_argument, &(args.show_export), 1},
    {"opcode", no_argument, &(args.show_opcode), 1},
    {NULL, 0, NULL, 0}
};

void usage() {
    puts("Usage: macho_parser [options] macho_file");
    puts("    -c, --command LOAD_COMMAND           show specific load command");
    puts("    -v, --verbose                        can be used multiple times to increase verbose level");
    puts("        --arch                           specify an architecture, arm64 or x86_64");
    puts("        --no-truncate                    do not truncate even the content is long");
    puts("    -h, --help                           show this help message");
    puts("");
    puts("    --segments                           equivalent to '--command LC_SEGMENT_64");
    puts("    --section INDEX                      show the section at INDEX");
    puts("    --dylibs                             show dylib related commands");
    puts("    --build-version                      equivalent to '--command LC_BUILD_VERSION --command LC_VERSION_MIN_*'");
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
    puts("");
    puts("Dyld Info Options:");
    puts("    --dyld-info                          equivalent to '--command LC_DYLD_INFO(_ONLY)'");
    puts("    --rebase                             show rebase info");
    puts("    --bind                               show binding info");
    puts("    --weak-bind                          show weak binding info");
    puts("    --lazy-bind                          show lazy binding info");
    puts("    --export                             show export trie");
    puts("    --opcode                             show the raw opcode instead of a table");
}

void parse_arguments(int argc, char **argv) {
    int opt = 0;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "c:s:hv", longopts, &option_index)) != -1) {
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
            case 's':
                args.sections[args.section_count++] = atoi(optarg);
                break;
            case 0:
                // all long options that don't have short options
                if (strcmp(longopts[option_index].name, "arch") == 0) {
                    args.arch = optarg;
                }
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

    if (args.arch != NULL && (strcasecmp(args.arch, "arm64") != 0 && strcasecmp(args.arch, "x86_64") != 0)) {
        fprintf(stderr, "Only architecture arm64 and x86_64 are supported.\n");
        exit(1);
    }

    if (args.show_segments || args.section_count > 0) {
        args.commands[args.command_count++] = LC_SEGMENT_64;

        if (args.section_count > 0) {
            args.verbosity += 1;
        }
    }

    if (args.show_dylibs) {
        args.commands[args.command_count++] = LC_ID_DYLIB;
        args.commands[args.command_count++] = LC_LOAD_DYLIB;
        args.commands[args.command_count++] = LC_LOAD_WEAK_DYLIB;
        args.commands[args.command_count++] = LC_REEXPORT_DYLIB;
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

    if (args.show_dyld_info || args.show_rebase || args.show_bind || args.show_weak_bind || args.show_lazy_bind || args.show_export) {
        args.commands[args.command_count++] = LC_DYLD_INFO;
        args.commands[args.command_count++] = LC_DYLD_INFO_ONLY;
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

bool show_header() {
    return args.command_count == 0;
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

bool show_section(int section) {
    if (args.section_count == 0) {
        // if no command is specified, show all sections
        return true;
    }

    bool show = false;
    for (int i = 0; i < args.section_count; ++i) {
        if (args.sections[i] == section) {
            show = true;
            break;
        }
    }
    return show;
}

bool is_selected_arch(char *arch) {
    if (args.arch == NULL) {
        // when --arch is not specified
        return true;
    }

    return strcasecmp(args.arch, arch) == 0;
}
