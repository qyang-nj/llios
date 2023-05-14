#include <stdio.h>
#include <iostream>

#include "utils/compression.h"
#include "llvm_cov.h"

extern "C"
{
  #include "util.h"
}

size_t printCovMapHeader(uint8_t *covMapBase);
size_t printFilenamesRegion(u_int8_t *filenamesBase);
void printFilenames(u_int8_t *uncompressedFileNames, int numFilenames);

// https://llvm.org/docs/CoverageMappingFormat.html

void printCovMapSection(uint8_t *base, struct section_64 *sect) {
    uint8_t *covMapBase = base + sect->offset;

    size_t offset = 0;
    while (offset < sect->size) {
        printf("\n");
        offset += printCovMapHeader(covMapBase + offset);
        offset += printFilenamesRegion(covMapBase + offset);
    }
}

// https://github.com/apple/llvm-project/blob/4305e61a0d81cc071a88090fa8579440c2220e07/llvm/include/llvm/ProfileData/Coverage/CoverageMapping.h#L990-L1007
size_t printCovMapHeader(uint8_t *covMapBase) {
    uint32_t * header = (uint32_t *)covMapBase;
    // https://github.com/apple/llvm-project/blob/4305e61a0d81cc071a88090fa8579440c2220e07/llvm/include/llvm/ProfileData/Coverage/CoverageMapping.h#L1011-L1029
    uint32_t version = header[3] + 1;
    printf("CovMap Header: (NRecords: %d, FilenamesSize: %d, CoverageSize: %d, Version: %d)\n", header[0], header[1], header[2], version);

    if (version < 4) {
        std::cerr << "Coverage map version lower than 4 is not supported." << std::endl;
        exit(1);
    }

    return 4 * 4; // CovMap header size
}

size_t printFilenamesRegion(uint8_t *filenamesBase) {
    uint64_t numFilenames = 0;
    uint64_t uncompressedLength = 0;
    uint64_t compressedLength = 0;

    size_t offset = 0;

    offset += read_uleb128(filenamesBase + offset, &numFilenames);
    offset += read_uleb128(filenamesBase + offset, &uncompressedLength);
    offset += read_uleb128(filenamesBase + offset, &compressedLength);

    printf("    Filenames: (NFilenames: %llu, UncompressedLen: %llu, CompressedLen: %llu)\n", numFilenames, uncompressedLength, compressedLength);

    uint8_t *uncompressedData = (uint8_t *)malloc(uncompressedLength);;
    if (compressedLength > 0) {
        decompressZlibData(filenamesBase + offset, compressedLength, uncompressedData, uncompressedLength);
        offset += compressedLength;
    } else {
        memcpy(uncompressedData, filenamesBase + offset, uncompressedLength);
        offset += uncompressedLength;
    }

    printFilenames(uncompressedData, numFilenames);

    free(uncompressedData);

    // Each coverage map has an alignment of 8 bytes
    return (offset + 7) / 8 * 8;
}

void printFilenames(uint8_t *uncompressedFileNames, int numFilenames) {
    int offset = 0;
    for (int i = 0; i < numFilenames; i++) {
        uint64_t filenameLength = 0;
        offset += read_uleb128(uncompressedFileNames + offset, &filenameLength);

        printf("     %2d: %.*s\n", i, (int)filenameLength, uncompressedFileNames + offset);
        offset += filenameLength;
    }
}
