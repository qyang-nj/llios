
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include "util.h"
}

#include "exports_trie.h"

static void printExport(uint8_t *base, uint32_t exportOff, uint32_t exportSize);

void printExportTrie(uint8_t *base, uint32_t dataoff, uint32_t datasize) {
    printExport(base, dataoff, datasize);
}

static void printExportRecursion(uint8_t *exportStart, uint8_t *nodePtr, int level) {
    uint64_t terminalSize;
    int byteCount = read_uleb128(nodePtr, &terminalSize);
    uint8_t *childrenCountPtr = nodePtr + byteCount + terminalSize;

    if (terminalSize != 0) {
        printf(" (data: ");
        for (int i = 0; i < terminalSize; ++i) {
            printf("%02x", *(nodePtr + byteCount + i));
        }
        printf(")\n");
    } else {
        printf("\n");
    }

    // According to the source code in dyld,
    // the count number is not uleb128 encoded;
    uint8_t children_count = *childrenCountPtr;
    uint8_t *s = childrenCountPtr + 1;
    for (int i = 0; i < children_count; ++i) {
        printf("  %*s%s", level * 2, "", s);
        s += strlen((char *)s) + 1;

        uint64_t child_offset;
        byteCount = read_uleb128(s, &child_offset);
        s += byteCount; // now s points to the next child's edge string
        printExportRecursion(exportStart, exportStart + child_offset, level + 1);
    }
}

static void printExport(uint8_t *base, uint32_t exportOff, uint32_t exportSize) {
    uint8_t *exportInfo = base + exportOff;
    printExportRecursion(exportInfo, exportInfo, 0);
}
