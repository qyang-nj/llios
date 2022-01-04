#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "small_cmds.h"

void printDyLinker(void *base, struct dylinker_command *dylinker_cmd) {
    printf("%-20s cmdsize: %-6u %s\n", "LC_LOAD_DYLINKER",
        dylinker_cmd->cmdsize, (char *)dylinker_cmd + dylinker_cmd->name.offset);
}

void printEntryPoint(void *base, struct entry_point_command *entry_point_cmd) {
    uint64_t entryoff = entry_point_cmd->entryoff;
    printf("%-20s cmdsize: %-6u entryoff: %llu (%#llx)  stacksize: %llu\n", "LC_MAIN",
        entry_point_cmd->cmdsize, entryoff, entryoff, entry_point_cmd->stacksize);
}

void printLinkerOption(void *base, struct linker_option_command *cmd) {
    char *options = (char *)calloc(1, cmd->cmdsize);
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

void printRpath(void *base, struct rpath_command *cmd) {
    printf("%-20s cmdsize: %-6u %s\n", "LC_RPATH", cmd->cmdsize, (char *)cmd + cmd->path.offset);
}

void printUUID(void *base, struct uuid_command *cmd) {
    printf("%-20s cmdsize: %-6u ", "LC_UUID", cmd->cmdsize);
    for (int i = 0; i < sizeof(cmd->uuid); ++i) {
        printf("%X", cmd->uuid[i]);
    }
    printf("\n");
}

void printSourceVersion(void *base, struct source_version_command *cmd) {
    int a = (0xFFFFFF0000000000 & cmd->version) >> 40;
    int b = (0x000000FFC0000000 & cmd->version) >> 30;
    int c = (0x000000003FF00000 & cmd->version) >> 20;
    int d = (0x00000000000FFC00 & cmd->version) >> 10;
    int e = (0x00000000000003FF & cmd->version);
    printf("%-20s cmdsize: %-6u %d.%d.%d.%d.%d\n", "LC_SOURCE_VERSION", cmd->cmdsize,
        a, b, c, d, e);
}
