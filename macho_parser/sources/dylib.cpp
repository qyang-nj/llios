#include <stdio.h>
#include <string.h>

extern "C" {
#include "build_version.h"
}

#include "load_command.h"
#include "argument.h"

static void printDylibDetail(struct dylib dylib);

void printDylib(const uint8_t *base, const struct dylib_command *cmd) {
    const char *cmdName = "";
    if (cmd->cmd == LC_ID_DYLIB) {
        cmdName = "LC_ID_DYLIB";
    } else if (cmd->cmd == LC_LOAD_DYLIB) {
        cmdName = "LC_LOAD_DYLIB";
    } else if (cmd->cmd == LC_LOAD_WEAK_DYLIB) {
        cmdName = "LC_LOAD_WEAK_DYLIB";
    } else if (cmd->cmd == LC_REEXPORT_DYLIB) {
        cmdName = "LC_REEXPORT_DYLIB";
    }
    printf("%-20s cmdsize: %-6u %s\n", cmdName, cmd->cmdsize, (char *)cmd + cmd->dylib.name.offset);

    if (args.verbosity < 2) {
        return;
    }

    printDylibDetail(cmd->dylib);
}

static void printDylibDetail(struct dylib dylib) {
    char current_version[32];
    char compatibility_version[32];

    format_version_string(dylib.current_version, current_version);
    format_version_string(dylib.compatibility_version, compatibility_version);

    printf("  timestamp            : %u\n", dylib.timestamp);
    printf("  current version      : %s\n", current_version);
    printf("  compatibility version : %s\n", compatibility_version);
}
