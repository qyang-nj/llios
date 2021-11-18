#ifndef UTIL_H
#define UTIL_H

#include <stdlib.h>

#define MIN(a,b) ((a) < (b) ? (a) : (b))

// Read a uleb128 number int to `out` and return the number of bytes processed.
// This method assumes the input correctness and doesn't handle error cases.
int read_uleb128(const uint8_t *p, uint64_t *out);

// If the string contains '\n', replace with literal "\n".
void format_string(char *str, char *formatted);

// Hex dump a binary buffer.
void format_hex(void *buffer, size_t size, char *formatted);

void format_size(uint64_t size_in_byte, char *formatted);

#endif /* UTIL_H */
