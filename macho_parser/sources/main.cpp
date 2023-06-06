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
#include "symtab.h"
#include "dysymtab.h"
}

#include "macho_header.h"
#include "macho_binary.h"
#include "load_command.h"
#include "dyld_info.h"
#include "encryption_info.h"
#include "small_cmds.h"
#include "ar_parser.h"
#include "build_version.h"

// dylib.cpp
void printDylib(const uint8_t *base, const struct dylib_command *cmd);

// segment_64.cpp
void printSegment(uint8_t *base, struct segment_command_64 *segCmd, int firstSectionIndex);

// linkedit_data.cpp
void printLinkEditData(uint8_t *base, struct linkedit_data_command *linkEditDataCmd);

// dysymtab.cpp
void printDynamicSymbolTable(uint8_t *base, struct dysymtab_command *dysymtabCmd);

static void printMacho(uint8_t *machoBase);
static void printLoadCommands(uint8_t *base, std::vector<struct load_command *> allLoadCommands);

struct MachoBinary machoBinary;

int main(int argc, char **argv) {
    parseArguments(argc, argv);

    int fd;
    struct stat sb;

    fd = open(args.file_name, O_RDONLY);
    fstat(fd, &sb);

    uint32_t fileSize = sb.st_size;
    uint8_t *fileBase = (uint8_t *)mmap(NULL, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (fileBase == MAP_FAILED) {
        fprintf(stderr, "Cannot read file %s\n", args.file_name);
        return 1;
    }

    uint8_t *sliceBase = fileBase;
    uint32_t sliceSize = fileSize;
    if (FatMacho::isFatMacho(fileBase, fileSize)) {
        std::tie(sliceBase, sliceSize) = FatMacho::getSliceByArch(fileBase, fileSize, args.arch);
    }

    if (Archive::isArchive(sliceBase, sliceSize)) { // handle static library
        Archive::enumerateObjectFileInArchive(sliceBase, sliceSize, [](char *objectFileName, uint8_t *objectFileBase) {
            printf("\033[0;34m%s:\033[0m\n", objectFileName);
            printMacho(objectFileBase);
            printf("\n");
        });
    } else {
        printMacho(sliceBase);
    }

    munmap(fileBase, fileSize);
    return 0;
}

static void printMacho(uint8_t *machoBase) {
    struct mach_header_64 *machHeader = parseMachHeader(machoBase);
    // the base address of a specific arch slice
    uint8_t *base = (uint8_t *)machHeader;
    std::vector<struct load_command *> allLoadCommands = parseLoadCommands(base, sizeof(struct mach_header_64), machHeader->ncmds);

    memset(&machoBinary, 0x0, sizeof(machoBinary));
    machoBinary.base = base;
    machoBinary.allLoadCommands = allLoadCommands;

    // filter segment commands
    std::vector<struct load_command *> segmentCommands;
    std::copy_if(allLoadCommands.begin(), allLoadCommands.end(), std::back_inserter(segmentCommands),
        [](struct load_command * lcmd){ return lcmd->cmd == LC_SEGMENT_64; });
    std::transform(segmentCommands.begin(), segmentCommands.end(), std::back_inserter(machoBinary.segmentCommands),
        [](struct load_command * lcmd){ return (struct segment_command_64 *)lcmd; });

    printLoadCommands(base, allLoadCommands);
}

static void printLoadCommands(uint8_t *base, std::vector<struct load_command *> allLoadCommands) {
    int sectionIndex = 0;

    for (struct load_command *lcmd : allLoadCommands) {

        if (!showCommand(lcmd->cmd)) {
            continue;
        }

        switch (lcmd->cmd) {
            case LC_SEGMENT_64:
                printSegment(base, (struct segment_command_64 *)lcmd, sectionIndex);
                sectionIndex += ((struct segment_command_64 *)lcmd)->nsects;
                break;
            case LC_SYMTAB:
                printSymbolTable(base, (struct symtab_command *)lcmd);
                break;
            case LC_DYSYMTAB:
                printDynamicSymbolTable(base, (struct dysymtab_command *)lcmd);
                break;
            case LC_LOAD_DYLINKER:
            case LC_ID_DYLINKER:
            case LC_DYLD_ENVIRONMENT:
                printDyLinker(base, (struct dylinker_command *)lcmd);
                break;
            case LC_MAIN:
                printEntryPoint(base, (struct entry_point_command *)lcmd);
                break;
            case LC_THREAD:
            case LC_UNIXTHREAD:
                printThread(base, (struct thread_command *)lcmd);
                break;
            case LC_LINKER_OPTION:
                printLinkerOption(base, (struct linker_option_command *)lcmd);
                break;
            case LC_ID_DYLIB:
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
                printDylib(base, (struct dylib_command *)lcmd);
                break;
            case LC_RPATH:
                printRpath(base, (struct rpath_command *)lcmd);
                break;
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY:
                printDyldInfo(base, (struct dyld_info_command *)lcmd);
                break;
            case LC_CODE_SIGNATURE:
            case LC_FUNCTION_STARTS:
            case LC_DATA_IN_CODE:
            case LC_DYLIB_CODE_SIGN_DRS:
            case LC_LINKER_OPTIMIZATION_HINT:
            case LC_DYLD_EXPORTS_TRIE:
            case LC_DYLD_CHAINED_FIXUPS:
            case LC_SEGMENT_SPLIT_INFO:
#if __clang_major__ >= 15
            case LC_ATOM_INFO:
#endif
                printLinkEditData(base, (struct linkedit_data_command *)lcmd);
                break;
            case LC_BUILD_VERSION:
                printBuildVersion(base, (struct build_version_command *)lcmd);
                break;
            case LC_VERSION_MIN_MACOSX:
            case LC_VERSION_MIN_IPHONEOS:
            case LC_VERSION_MIN_WATCHOS:
            case LC_VERSION_MIN_TVOS:
                printVersionMin(base, (struct version_min_command *)lcmd);
                break;
            case LC_UUID:
                printUUID(base, (struct uuid_command *)lcmd);
                break;
            case LC_SOURCE_VERSION:
                printSourceVersion(base, (struct source_version_command *)lcmd);
                break;
            case LC_ENCRYPTION_INFO_64:
                printEncryptionInfo(base, (struct encryption_info_command_64 *)lcmd);
                break;
            default:
                printf("LC_(%x)\n", lcmd->cmd);
        }
    }
}
