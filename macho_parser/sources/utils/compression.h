#ifndef MACHO_PARSER_COMPRESSION_H
#define MACHO_PARSER_COMPRESSION_H

// Decompress that data using zlib.
void decompressZlibData(u_int8_t* inputData, size_t inputSize, u_int8_t* outputData, size_t outputSize);

#endif /* MACHO_PARSER_COMPRESSION_H */
