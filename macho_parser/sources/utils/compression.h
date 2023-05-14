#ifndef MACHO_PARSER_COMPRESSION_H
#define MACHO_PARSER_COMPRESSION_H

// Decompress that data using zlib.
void decompressZlibData(const uint8_t *inputData, size_t inputSize, uint8_t *outputData, size_t outputSize);

#endif /* MACHO_PARSER_COMPRESSION_H */
