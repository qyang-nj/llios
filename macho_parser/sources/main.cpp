#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <vector>

extern "C" {
#include "argument.h"
#include "util.h"
#include "segment_64.h"
#include "symtab.h"
#include "dysymtab.h"
#include "dyld_info.h"
#include "dylib.h"
#include "linkedit_data.h"
#include "build_version.h"
}

#include "macho_header.h"
#include "macho_binary.h"
#include "load_command.h"

static void printLoadCommands(uint8_t *base, std::vector<struct load_command *> allLoadCommands);

void parse_dylinker(void *base, struct dylinker_command *);
void parse_entry_point(void *base, struct entry_point_command *);
void parse_linker_option(void *base, struct linker_option_command *);
void parse_rpath(void *base, struct rpath_command *);
void parse_uuid(void *base, struct uuid_command *cmd);
void parse_source_version(void *base, struct source_version_command *cmd);

struct MachoBinary machoBinary;

int main(int argc, char **argv) {
    parse_arguments(argc, argv);

    int fd;
    struct stat sb;

    fd = open(args.file_name, O_RDONLY);
    fstat(fd, &sb);

    uint8_t *fileBase = (uint8_t *)mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (fileBase == MAP_FAILED) {
        fprintf(stderr, "Cannot read file %s\n", args.file_name);
        return 1;
    }

    struct mach_header_64 *machHeader = parseMachHeader(fileBase);
    // the base address of a specific arch slice
    uint8_t *base = (uint8_t *)machHeader;
    static std::vector<struct load_command *> allLoadCommands = parseLoadCommands(base, sizeof(struct mach_header_64), machHeader->ncmds);

    machoBinary.base = base;
    machoBinary.allLoadCommands = allLoadCommands;

    // filter segment commands
    std::vector<struct load_command *> segmentCommands;
    std::copy_if(allLoadCommands.begin(), allLoadCommands.end(), std::back_inserter(segmentCommands),
        [](struct load_command * lcmd){ return lcmd->cmd == LC_SEGMENT_64; });
    std::transform(segmentCommands.begin(), segmentCommands.end(), std::back_inserter(machoBinary.segmentCommands),
        [](struct load_command * lcmd){ return (struct segment_command_64 *)lcmd; });

    printLoadCommands(base, allLoadCommands);

    munmap(base, sb.st_size);
    return 0;
}

static void printLoadCommands(uint8_t *base, std::vector<struct load_command *> allLoadCommands) {
    int sectionIndex = 0;

    for (struct load_command *lcmd : allLoadCommands) {

        if (!show_command(lcmd->cmd)) {
            continue;
        }

        switch (lcmd->cmd) {
            case LC_SEGMENT_64:
                parse_segment(base, (struct segment_command_64 *)lcmd, sectionIndex);
                sectionIndex += ((struct segment_command_64 *)lcmd)->nsects;
                break;
            case LC_SYMTAB:
                parse_symbol_table(base, (struct symtab_command *)lcmd);
                break;
            case LC_DYSYMTAB:
                parse_dynamic_symbol_table(base, (struct dysymtab_command *)lcmd);
                break;
            case LC_LOAD_DYLINKER:
                parse_dylinker(base, (struct dylinker_command *)lcmd);
                break;
            case LC_MAIN:
                parse_entry_point(base, (struct entry_point_command *)lcmd);
                break;
            case LC_LINKER_OPTION:
                parse_linker_option(base, (struct linker_option_command *)lcmd);
                break;
            case LC_ID_DYLIB:
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
                parse_dylib(base, (struct dylib_command *)lcmd);
                break;
            case LC_RPATH:
                parse_rpath(base, (struct rpath_command *)lcmd);
                break;
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY:
                parse_dyld_info(base, (struct dyld_info_command *)lcmd);
                break;
            case LC_CODE_SIGNATURE:
            case LC_FUNCTION_STARTS:
            case LC_DATA_IN_CODE:
            case LC_DYLIB_CODE_SIGN_DRS:
            case LC_LINKER_OPTIMIZATION_HINT:
            case LC_DYLD_EXPORTS_TRIE:
            case LC_DYLD_CHAINED_FIXUPS:
                parse_linkedit_data(base, (struct linkedit_data_command *)lcmd);
                break;
            case LC_BUILD_VERSION:
                parse_build_version(base, (struct build_version_command *)lcmd);
                break;
            case LC_VERSION_MIN_MACOSX:
            case LC_VERSION_MIN_IPHONEOS:
            case LC_VERSION_MIN_WATCHOS:
            case LC_VERSION_MIN_TVOS:
                parse_version_min(base, (struct version_min_command *)lcmd);
                break;
            case LC_UUID:
                parse_uuid(base, (struct uuid_command *)lcmd);
                break;
            case LC_SOURCE_VERSION:
                parse_source_version(base, (struct source_version_command *)lcmd);
                break;
            default:
                printf("LC_(%x)\n", lcmd->cmd);
        }
    }
}

void parse_dylinker(void *base, struct dylinker_command *dylinker_cmd) {
    printf("%-20s cmdsize: %-6u %s\n", "LC_LOAD_DYLINKER",
        dylinker_cmd->cmdsize, (char *)dylinker_cmd + dylinker_cmd->name.offset);
}

void parse_entry_point(void *base, struct entry_point_command *entry_point_cmd) {
    uint64_t entryoff = entry_point_cmd->entryoff;
    printf("%-20s cmdsize: %-6u entryoff: 0x%llx(%llu) stacksize: %llu\n", "LC_MAIN",
        entry_point_cmd->cmdsize, entryoff, entryoff, entry_point_cmd->stacksize);
}

void parse_linker_option(void *base, struct linker_option_command *cmd) {
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

void parse_rpath(void *base, struct rpath_command *cmd) {
    printf("%-20s cmdsize: %-6u %s\n", "LC_RPATH", cmd->cmdsize, (char *)cmd + cmd->path.offset);
}

void parse_uuid(void *base, struct uuid_command *cmd) {
    printf("%-20s cmdsize: %-6u ", "LC_UUID", cmd->cmdsize);
    for (int i = 0; i < sizeof(cmd->uuid); ++i) {
        printf("%X", cmd->uuid[i]);
    }
    printf("\n");
}

void parse_source_version(void *base, struct source_version_command *cmd) {
    int a = (0xFFFFFF0000000000 & cmd->version) >> 40;
    int b = (0x000000FFC0000000 & cmd->version) >> 30;
    int c = (0x000000003FF00000 & cmd->version) >> 20;
    int d = (0x00000000000FFC00 & cmd->version) >> 10;
    int e = (0x00000000000003FF & cmd->version);
    printf("%-20s cmdsize: %-6u %d.%d.%d.%d.%d\n", "LC_SOURCE_VERSION", cmd->cmdsize,
        a, b, c, d, e);
}


