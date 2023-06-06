#include <stdio.h>
#include <string.h>

#include "utils/utils.h"
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
    auto currentVersion = formatVersion(dylib.current_version);
    auto compatibilityVersion = formatVersion(dylib.compatibility_version);

    printf("  timestamp            : %u\n", dylib.timestamp);
    printf("  current version      : %s\n", currentVersion.c_str());
    printf("  compatibility version : %s\n", compatibilityVersion.c_str());
}
