#ifndef MACHO_PARSER_UTILS_H
#define MACHO_PARSER_UTILS_H

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read a uleb128 number int to `out` and return the number of bytes processed.
// This method assumes the input correctness and doesn't handle error cases.
int readULEB128(const uint8_t *p, uint64_t *out);

// Read a sleb128 number int to `out` and return the number of bytes processed.
// This method assumes the input correctness and doesn't handle error cases.
int readSLEB128(const uint8_t *p, int64_t *out);

#ifdef __cplusplus
}
#endif


#endif //MACHO_PARSER_UTILS_H
