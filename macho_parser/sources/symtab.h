#ifndef SYMTAB_H
#define SYMTAB_H

#include <mach-o/loader.h>
#include <stdbool.h>

void printSymbolTable(uint8_t *base, struct symtab_command *cmd);

void printSymbol(int indent, uint8_t *base, struct symtab_command *symtabCmd, int offset);

char *lookup_symbol_by_address(uint64_t address, uint8_t *base, struct symtab_command *symtabCmd);

bool is_symtab_load_command(struct load_command *lcmd);

#endif /* SYMTAB_H */
