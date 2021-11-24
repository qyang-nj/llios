#include <stdio.h>

#include "argument.h"
#include "build_version.h"

#include "dylib.h"

static void print_dylib_detail(struct dylib dylib);

void parse_dylib(void *base, struct dylib_command *cmd) {
    char *cmd_name = "";
    if (cmd->cmd == LC_ID_DYLIB) {
        cmd_name = "LC_ID_DYLIB";
    } else if (cmd->cmd == LC_LOAD_DYLIB) {
        cmd_name = "LC_LOAD_DYLIB";
    } else if (cmd->cmd == LC_LOAD_WEAK_DYLIB) {
        cmd_name = "LC_LOAD_WEAK_DYLIB";
    } else if (cmd->cmd == LC_REEXPORT_DYLIB) {
        cmd_name = "LC_REEXPORT_DYLIB";
    }
    printf("%-20s cmdsize: %-6u %s\n", cmd_name, cmd->cmdsize, (char *)cmd + cmd->dylib.name.offset);

    if (args.verbosity < 2) {
        return;
    }

    print_dylib_detail(cmd->dylib);
}

static void print_dylib_detail(struct dylib dylib) {
    char current_version[32];
    char compatibility_version[32];

    format_version_string(dylib.current_version, current_version);
    format_version_string(dylib.compatibility_version, compatibility_version);

    printf("  timestamp            : %u\n", dylib.timestamp);
    printf("  current version      : %s\n", current_version);
    printf("  compatibilityversion : %s\n", compatibility_version);
}
