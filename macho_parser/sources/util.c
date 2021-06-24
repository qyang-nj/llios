#include "util.h"

void read_bytes(FILE *fptr, int offset, void *buf, int size) {
    fseek(fptr, offset, SEEK_SET);
    fread(buf, size, 1, fptr);
}

void *load_bytes(FILE *fptr, int offset, int size) {
    void *buf = calloc(1, size);
    read_bytes(fptr, offset, buf, size);
    return buf;
}

int read_uleb128(const uint8_t *p, uint64_t *out) {
    uint64_t result = 0;
    int i = 0;

    do {
        uint8_t byte = *p & 0x7f;
        result |= byte << (i * 7);
        i++;
    } while (*p++ & 0x80);

    *out = result;
    return i;
}

void format_string(char *str, char *formatted) {
    int j = 0;
    for (int i = 0; str[i] != '\0'; ++i) {
        switch(str[i]) {
            case '\n':
                formatted[j++] = '\\';
                formatted[j++] = 'n';
                break;
            default:
                formatted[j++] = str[i];
                break;
        }
    }
    formatted[j] = '\0';
}
