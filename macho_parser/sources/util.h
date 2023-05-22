#ifndef UTIL_H
#define UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>

#define MIN(a,b) ((a) < (b) ? (a) : (b))

// If the string contains '\n', replace with literal "\n".
void format_string(char *str, char *formatted);

// Hex dump a binary buffer.
void format_hex(void *buffer, size_t size, char *formatted);

void format_size(uint64_t size_in_byte, char *formatted);

#ifdef __cplusplus
}
#endif

#endif /* UTIL_H */
