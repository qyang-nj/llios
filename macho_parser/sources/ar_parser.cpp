#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ar.h>

#include "ar_parser.h"

// This file handles parsing archive (static library) format

bool isArchive(uint8_t *fileBase) {
    return strncmp(ARMAG,(char *)fileBase, strlen(ARMAG)) == 0;
}

void enumerateObjectFileInArchive(uint8_t *archiveBase, size_t fileSize, std::function<void(char*, uint8_t*)> const& handler) {
    assert(isArchive(archiveBase));

    uint32_t offset = strlen(ARMAG);
    while (offset < fileSize) {
        struct ar_hdr *metadata = (struct ar_hdr *)(archiveBase + offset);
        assert(strncmp(ARFMAG, metadata->ar_fmag, strlen(ARFMAG)) == 0);
        offset += sizeof(struct ar_hdr);

        char *fileName = metadata->ar_name;
        int efmtSize = 0;
        if (strncmp(AR_EFMT1, fileName, strlen(AR_EFMT1)) == 0) {
            efmtSize = atoi((char *)(fileName + strlen(AR_EFMT1)));
            fileName = (char *)(archiveBase + offset);
        }

        int fileSize = atoi(metadata->ar_size);

        // The first member in a static archive library is always the symbol table describing the contents of the rest of the member files.
        // It's always called __.SYMDEF or __.SYMDEF SORTED. We just skip this.
        // http://mirror.informatimago.com/next/developer.apple.com/documentation/DeveloperTools/Conceptual/MachORuntime/8rt_file_format/chapter_10_section_33.html
        if (strncmp("__.SYMDEF", fileName, strlen("__.SYMDEF")) != 0
            && strncmp("__.SYMDEF SORTED", fileName, strlen("__.SYMDEF SORTED")) != 0) {
            handler(fileName, archiveBase + offset + efmtSize);
        }

        offset += fileSize;
    }
}
