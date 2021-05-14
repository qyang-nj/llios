#include <stdlib.h>
#include "argument.h"
#include "dyld_info.h"

extern void *load_bytes(FILE *fptr, int offset, int size);

void parse_export(FILE *fptr, uint32_t export_off, uint32_t export_size);
void parse_export_dfs(uint8_t *export_start, uint8_t *node_ptr, char *curr_string, int curr_len);
int read_uleb128(const uint8_t *p, uint64_t *out);

void parse_dyld_info(FILE *fptr, struct dyld_info_command *dyld_info_cmd) {
    const char *name = (dyld_info_cmd->cmd == LC_DYLD_INFO_ONLY ? "LC_DYLD_INFO_ONLY" : "LC_DYLD_INFO_ONLY");
    printf("%-20s cmdsize: %-6u\n", name, dyld_info_cmd->cmdsize);

    if (args.short_desc) { return; }

    printf("    rebase_off   : 0x%08x   rebase_size   : %d\n", dyld_info_cmd->rebase_off, dyld_info_cmd->rebase_size);
    printf("    bind_off     : 0x%08x   bind_size     : %d\n", dyld_info_cmd->bind_off, dyld_info_cmd->bind_size);
    printf("    weak_bind_off: 0x%08x   weak_bind_size: %d\n", dyld_info_cmd->weak_bind_off, dyld_info_cmd->weak_bind_size);
    printf("    lazy_bind_off: 0x%08x   lazy_bind_size: %d\n", dyld_info_cmd->lazy_bind_off, dyld_info_cmd->lazy_bind_size);
    printf("    export_off   : 0x%08x   export_size   : %d\n", dyld_info_cmd->export_off, dyld_info_cmd->export_size);

    parse_export(fptr, dyld_info_cmd->export_off, dyld_info_cmd->export_size);
}

void parse_export(FILE *fptr, uint32_t export_off, uint32_t export_size) {
    uint8_t *export = load_bytes(fptr, export_off, export_size);
    uint8_t *p = export;

    printf ("\n    Exported Symbols:\n");
    char symbol[4096];
    parse_export_dfs(export, export, symbol, 0);

    free(export);
}

// This method only passes the symbols in the trie, not the associated data.
// For detailed export trie parsing, see MachOTrie.hpp in dyld.
void parse_export_dfs(uint8_t *export_start, uint8_t *node_ptr, char *curr_string, int curr_len) {
    uint64_t terminal_size;
    int byte_count = read_uleb128(node_ptr, &terminal_size);
    uint8_t *children_count_ptr = node_ptr + byte_count + terminal_size;

    if (terminal_size != 0) {
        printf("        %s (data: ", curr_string);
        for (int i = 0; i < terminal_size; ++i) {
            printf("%x", *(node_ptr + byte_count + i));
        }
        printf(")\n");
    }

    // According to the source code in dyld,
    // the count number is not uleb128 encoded;
    uint8_t children_count = *children_count_ptr;
    uint8_t *s = children_count_ptr + 1;
    for (int i = 0; i < children_count; ++i) {
        int edge_len = 0;
        while (*s != '\0') {
            curr_string[curr_len + edge_len] = *s;
            ++s;
            ++edge_len;
        }

        curr_string[curr_len + edge_len] = '\0';
        s++;

        uint64_t child_offset;
        byte_count = read_uleb128(s, &child_offset);
        s += byte_count; // now s points to the next child's edge string
        parse_export_dfs(export_start, export_start + child_offset, curr_string, curr_len + edge_len);
    }
}

// Read a uleb128 number int to out, return the number of bytes processed.
// This method assumes the input correctness and doesn't handle error cases,
// like integer overflow or malformed uleb128.
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
