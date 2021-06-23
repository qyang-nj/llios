#ifndef ARGUMENT_H
#define ARGUMENT_H

#include <mach-o/loader.h>
#include <stdbool.h>

// command line argument
struct argument {
    // -s: show a short one-line description for each load command
    bool short_desc;
    // -v: verbose level
    int verbose;
    // -c <cmd>: show specified command
    unsigned int commands[12];
    // the number of specified commands
    int command_count;
    // filename of the mach-o file
    char *file_name;
};

// global variable that holds command line arguments
extern struct argument args;

// parse the command line arguments and save the result to args
void parse_arguments(int argc, char **argv);

// convert a string to the load command number, "LC_SYMTAB" -> 0x2 (LC_SYMTAB)
unsigned int string_to_load_command(char *cmd_str);

// whether to show a command
bool show_command(unsigned int cmd);

#endif /* ARGUMENT_H */
