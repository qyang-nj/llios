#include <stdlib.h>
#include "argument.h"
#include "dyld_info.h"

extern void *load_bytes(FILE *fptr, int offset, int size);

void parse_dyld_info(FILE *fptr, struct dyld_info_command *dyld_info_cmd) {
    const char *name = (dyld_info_cmd->cmd == LC_DYLD_INFO_ONLY ? "LC_DYLD_INFO_ONLY" : "LC_DYLD_INFO_ONLY");
    printf("%-20s cmdsize: %-6u\n", name, dyld_info_cmd->cmdsize);

    if (args.short_desc) { return; }

    printf("    rebase_off   : 0x%08x   rebase_size   : %d\n", dyld_info_cmd->rebase_off, dyld_info_cmd->rebase_size);
    printf("    bind_off     : 0x%08x   bind_size     : %d\n", dyld_info_cmd->bind_off, dyld_info_cmd->bind_size);
    printf("    weak_bind_off: 0x%08x   weak_bind_size: %d\n", dyld_info_cmd->weak_bind_off, dyld_info_cmd->weak_bind_size);
    printf("    lazy_bind_off: 0x%08x   lazy_bind_size: %d\n", dyld_info_cmd->lazy_bind_off, dyld_info_cmd->lazy_bind_size);
    printf("    export_off   : 0x%08x   export_size   : %d\n", dyld_info_cmd->export_off, dyld_info_cmd->export_size);
}

void parse_export(FILE *fptr, uint32_t export_off, uint32_t export_size) {
    uint8_t *export = load_bytes(fptr, export_off, export_size);


    free(export);
}
