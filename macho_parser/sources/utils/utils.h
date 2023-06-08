#ifndef MACHO_PARSER_UTILS_H
#define MACHO_PARSER_UTILS_H

#include <stdlib.h>
#include <string>

// Read a uleb128 number int to `out` and return the number of bytes processed.
// This method assumes the input correctness and doesn't handle error cases.
int readULEB128(const uint8_t *p, uint64_t *out);

// Read a sleb128 number int to `out` and return the number of bytes processed.
// This method assumes the input correctness and doesn't handle error cases.
int readSLEB128(const uint8_t *p, int64_t *out);

// Decompress that data using zlib.
void decompressZlibData(const uint8_t *inputData, size_t inputSize, uint8_t *outputData, size_t outputSize);

// Hex dump a range of memory to stdout.
void hexdump(uint32_t start, const void* data, size_t size);

// formatting
std::string formatSize(uint64_t sizeInByte);
std::string formatBufferToHex(const uint8_t *buffer, size_t bufferSize);
std::string formatStringLiteral(const char *str);
std::string formatVersion(uint32_t version);

#endif //MACHO_PARSER_UTILS_H
