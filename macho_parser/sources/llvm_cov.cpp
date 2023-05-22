#include <stdio.h>
#include <iostream>
#include <vector>

#include "utils/utils.h"
#include "llvm_cov.h"

// https://llvm.org/docs/CoverageMappingFormat.html

// BEGIN __llvm_covmap

static size_t printCovMapHeader(uint8_t *covMapBase);
static size_t printFilenamesRegion(uint8_t *filenamesBase);
static void printFilenames(uint8_t *uncompressedFileNames, int numFilenames);

void printCovMapSection(uint8_t *base, struct section_64 *sect) {
    uint8_t *covMapBase = base + sect->offset;

    int index = 0;

    size_t offset = 0;
    while (offset < sect->size) {
        std::cout << "  === " << index++ << " ===" << std::endl;
        offset += printCovMapHeader(covMapBase + offset);
        offset += printFilenamesRegion(covMapBase + offset);
        std::cout << std::endl;
    }
}

// https://github.com/apple/llvm-project/blob/4305e61a0d81cc071a88090fa8579440c2220e07/llvm/include/llvm/ProfileData/Coverage/CoverageMapping.h#L990-L1007
static size_t printCovMapHeader(uint8_t *covMapBase) {
    uint32_t * header = (uint32_t *)covMapBase;
    // https://github.com/apple/llvm-project/blob/4305e61a0d81cc071a88090fa8579440c2220e07/llvm/include/llvm/ProfileData/Coverage/CoverageMapping.h#L1011-L1029
    uint32_t version = header[3] + 1;
    printf("  CovMap Header: (NRecords: %d, FilenamesSize: %d, CoverageSize: %d, Version: %d)\n", header[0], header[1], header[2], version);

    if (version < 4) {
        std::cerr << "Coverage map version lower than 4 is not supported." << std::endl;
        exit(1);
    }

    return 4 * 4; // CovMap header size
}

static size_t printFilenamesRegion(uint8_t *filenamesBase) {
    uint64_t numFilenames = 0;
    uint64_t uncompressedLength = 0;
    uint64_t compressedLength = 0;

    size_t offset = 0;

    offset += readULEB128(filenamesBase + offset, &numFilenames);
    offset += readULEB128(filenamesBase + offset, &uncompressedLength);
    offset += readULEB128(filenamesBase + offset, &compressedLength);

    printf("  Filenames: (NFilenames: %llu, UncompressedLen: %llu, CompressedLen: %llu)\n", numFilenames, uncompressedLength, compressedLength);

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

static void printFilenames(uint8_t *uncompressedFileNames, int numFilenames) {
    int offset = 0;
    for (int i = 0; i < numFilenames; i++) {
        uint64_t filenameLength = 0;
        offset += readULEB128(uncompressedFileNames + offset, &filenameLength);

        printf("    %2d: %.*s\n", i, (int)filenameLength, uncompressedFileNames + offset);
        offset += filenameLength;
    }
}

// END __llvm_covmap

// BEGIN __llvm_covfun

static void printFunctionEncoding(uint8_t *funcEncodingBase);
static size_t printFileIDMapping(uint8_t *fileIDMappingBase, int *numFiles);
static size_t parseCounterExpressions(uint8_t *counterExpressionBase, std::vector<std::pair<uint64_t, uint64_t>> &counterExpressions);
static size_t printMappingRegions(uint8_t *mappingRegionBase, int numFiles, const std::vector<std::pair<uint64_t, uint64_t>> &counterExpressions);
static std::string formatCounter(uint64_t counter, const std::vector<std::pair<uint64_t, uint64_t>> &counterExpressions);

void printCovFunSection(uint8_t *base, struct section_64 *sect) {
    uint8_t *covFunBase = base + sect->offset;

    int index = 0;

    size_t offset = 0;
    while (offset < sect->size) {
        // The hashes here are the lower 64 bits of the MD5 hash
        int64_t funcNameHash = *(int64_t *)(covFunBase + offset);
        offset += sizeof(int64_t);
        int32_t dataLen = *(int32_t *)(covFunBase + offset);
        offset += sizeof(int32_t);
        int64_t funcHash = *(int64_t *)(covFunBase + offset);
        offset += sizeof(int64_t);
        int64_t fileNameHash = *(int64_t *)(covFunBase + offset);
        offset += sizeof(int64_t);

        printf("%d: FuncNameHash: 0x%llx, DataLen: %d, FuncHash: 0x%llx, FileNameHash: 0x%llx\n", index++, funcNameHash, dataLen, funcHash, fileNameHash);
        printFunctionEncoding(covFunBase + offset);

        offset += dataLen;
        offset = (offset + 7) / 8 * 8; // align to 8 bytes
    }
}

static void printFunctionEncoding(uint8_t *funcEncodingBase) {
    std::vector<std::pair<uint64_t, uint64_t>> counterExpressions;
    int numFiles = 0;
    size_t offset = printFileIDMapping(funcEncodingBase, &numFiles);
    offset += parseCounterExpressions(funcEncodingBase + offset, counterExpressions);
    offset += printMappingRegions(funcEncodingBase + offset, numFiles, counterExpressions);
}

static size_t printFileIDMapping(uint8_t *fileIDMappingBase, int *numFiles) {
    uint64_t numIndices = 0;
    size_t offset = readULEB128(fileIDMappingBase, &numIndices);

    printf("    FileIDMapping: (NFiles: %llu)\n", numIndices);

    for (int i = 0; i < numIndices; i++) {
        uint64_t filenameIndex = 0;
        offset += readULEB128(fileIDMappingBase + offset, &filenameIndex);

        printf("     %2d: %llu\n", i, filenameIndex);
    }

    *numFiles = numIndices;
    return offset;
}

// Parse counter expressions into a vector of pairs of (LHS, RHS) and return the bytes that have been processed
static size_t parseCounterExpressions(uint8_t *counterExpressionBase, std::vector<std::pair<uint64_t, uint64_t>> &counterExpressions) {
    uint64_t numExpressions = 0;
    size_t offset = readULEB128(counterExpressionBase, &numExpressions);

    for (int i = 0; i < numExpressions; i++) {
        uint64_t exprLHS = 0, exprRHS = 0;
        offset += readULEB128(counterExpressionBase + offset, &exprLHS);
        offset += readULEB128(counterExpressionBase + offset, &exprRHS);

        counterExpressions.push_back(std::make_pair(exprLHS, exprRHS));
    }

    return offset;
}

static size_t printMappingRegions(uint8_t *mappingRegionBase, int numFiles, const std::vector<std::pair<uint64_t, uint64_t>> &counterExpressions) {
    size_t offset = 0;

    printf("    MappingRegions: (NRegionArrays: %d)\n", numFiles);

    for (int i = 0; i < numFiles; i++) {
        uint64_t numRegions = 0;
        offset += readULEB128(mappingRegionBase + offset, &numRegions);

        printf("     %2d: (NRegions: %llu)\n", i, numRegions);

        int prevLinStart = 0;
        for (int j = 0; j < numRegions; j++) {
            uint64_t counter = 0;
            offset += readULEB128(mappingRegionBase + offset, &counter);

            uint64_t deltaLineStart = 0, columnStart = 0, numLines = 0, columnEnd = 0;
            offset += readULEB128(mappingRegionBase + offset, &deltaLineStart);
            offset += readULEB128(mappingRegionBase + offset, &columnStart);
            offset += readULEB128(mappingRegionBase + offset, &numLines);
            offset += readULEB128(mappingRegionBase + offset, &columnEnd);

            int lineStart = (j == 0 ? deltaLineStart : prevLinStart + deltaLineStart);

            // Map region to counter
            printf("         %d: %d:%llu => %llu:%llu : ", j, lineStart, columnStart, lineStart + numLines, columnEnd);
            std::cout << formatCounter(counter, counterExpressions) << std::endl;

            prevLinStart = lineStart;
        }
    }

    return offset;
}

// Format the counter into a string.
static std::string formatCounter(uint64_t counter, const std::vector<std::pair<uint64_t, uint64_t>> &counterExpressions) {
    std::string result = "";

    // The lower 2 bits of the counter are used to encode the tag of the counter.
    uint8_t tag = counter & 0x3;
    uint64_t counterIndex = counter >> 2;

    if (tag == 0) {
        result += "pseudo-counter";
    } else if (tag == 1) {
        // The counter is a reference to the profile instrumentation counter.
        result += std::to_string(counterIndex);
    } else {
        // The counter is a subtraction or addition expression.
        std::pair<uint64_t, uint64_t> expression = counterExpressions[counterIndex];
        uint64_t lhs = expression.first;
        uint64_t rhs = expression.second;
        result += "(";
        result += formatCounter(lhs, counterExpressions);
        result += tag == 2 ? " - " : " + ";
        result += formatCounter(rhs, counterExpressions);
        result += ")";
    }

    return result;
}

// END __llvm_covfun

// BEGIN __llvm_prf_names
std::vector<std::string> splitString(const std::string& input, char delimiter);

void printPrfNamesSection(uint8_t *base, struct section_64 *sect) {
    uint8_t *prfNamesBase = base + sect->offset;

    int index = 0;

    size_t offset = 0;
    while (offset < sect->size) {
        uint64_t uncompressedSize = 0;
        offset += readULEB128(prfNamesBase + offset, &uncompressedSize);

        uint64_t compressedSize = 0;
        offset += readULEB128(prfNamesBase + offset, &compressedSize);

        uint8_t *uncompressedData = (uint8_t *)malloc(uncompressedSize);;
        if (compressedSize > 0) {
            decompressZlibData(prfNamesBase + offset, compressedSize, uncompressedData, uncompressedSize);
            offset += compressedSize;
        } else {
            memcpy(uncompressedData, prfNamesBase + offset, uncompressedSize);
            offset += uncompressedSize;
        }

        std::cout << "  === " << index++ << " ===" << std::endl;

        auto names = splitString((char *)uncompressedData, '\1');
        for (auto name : names) {
            std::cout << "  " << name << std::endl;
        }

        free(uncompressedData);
    }
}

std::vector<std::string> splitString(const std::string& input, char delimiter) {
    std::vector<std::string> result;
    std::string substring;
    std::size_t startPos = 0;
    std::size_t endPos = 0;

    while ((endPos = input.find(delimiter, startPos)) != std::string::npos) {
        substring = input.substr(startPos, endPos - startPos);
        result.push_back(substring);
        startPos = endPos + 1;
    }

    // Push the remaining string after the last delimiter
    substring = input.substr(startPos);
    result.push_back(substring);

    return result;
}

// END __llvm_prf_names
