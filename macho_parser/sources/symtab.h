#ifndef SYMTAB_H
#define SYMTAB_H

#include <mach-o/loader.h>

void parse_symbol_table(void *base, struct symtab_command *cmd);

#endif /* SYMTAB_H */