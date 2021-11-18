#ifndef SYMTAB_H
#define SYMTAB_H

#include <mach-o/loader.h>
#include <stdbool.h>

void parse_symbol_table(void *base, struct symtab_command *cmd);

void print_symbol(int indent, void *base, struct symtab_command *symtab_cmd, int offset);

char *lookup_symbol_by_address(uint64_t address, void *base, struct symtab_command *symtab_cmd);

bool is_symtab_load_command(struct load_command *lcmd);

#endif /* SYMTAB_H */
