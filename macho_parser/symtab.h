#ifndef SYMTAB_H
#define SYMTAB_H

#include <stdio.h>
#include <mach-o/loader.h>

void parse_symbol_table(FILE *fptr, struct symtab_command *cmd);

#endif /* SYMTAB_H */
