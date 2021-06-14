#include "util.h"

void *load_bytes(FILE *fptr, int offset, int size) {
    void *buf = calloc(1, size);
    fseek(fptr, offset, SEEK_SET);
    fread(buf, size, 1, fptr);
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

// If the string contains '\n', replace with literal "\n".s
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
