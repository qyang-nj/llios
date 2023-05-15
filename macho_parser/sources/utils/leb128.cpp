#include "utils.h"

int readULEB128(const uint8_t *p, uint64_t *out) {
    uint64_t result = 0;
    int i = 0;

    do {
        uint8_t byte = *p & 0x7f;
        result |= (uint64_t)byte << (i * 7);
        i++;
    } while (*p++ & 0x80);

    *out = result;
    return i;
}

int readSLEB128(const uint8_t *p, int64_t *out) {
    int64_t result = 0;
    int i = 0;
    uint8_t byte;

    do {
        byte = *p & 0x7f;
        result |= byte << (i * 7);
        i++;
    } while (*p++ & 0x80);

    // The sign bit is set
    if (byte & 0x40) {
        result |= (~0 << (i * 7));
    }

    *out = result;
    return i;
}
