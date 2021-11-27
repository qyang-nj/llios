#ifndef ARGUMENT_H
#define ARGUMENT_H

#include <mach-o/loader.h>
#include <stdbool.h>

// command line argument
struct argument {
    char *file_name;
    int verbosity;
    unsigned int commands[12];
    int command_count;
    int no_truncate;

    int show_build_version;
    int show_segments;
    int section_count;
    int sections[12];
    int show_dylibs;

    // code signature options
    int show_code_signature;
    int show_code_direcotry;
    int show_entitlement;
    int show_blob_wrapper;

    // dynamic symbol table options
    int show_dysymtab;
    int show_local;
    int show_extdef;
    int show_undef;
    int show_indirect;

    // dyld info options
    int show_dyld_info;
    int show_rebase;
    int show_bind;
    int show_weak_bind;
    int show_lazy_bind;
    int show_export;
    int show_opcode;
};

// global variable that holds command line arguments
extern struct argument args;

// parse the command line arguments and save the result to args
void parse_arguments(int argc, char **argv);

// convert a string to the load command number, "LC_SYMTAB" -> 0x2 (LC_SYMTAB)
unsigned int string_to_load_command(char *cmd_str);

// whether to show header
bool show_header();

// whether to show a command
bool show_command(unsigned int cmd);

bool show_section(int section);

#endif /* ARGUMENT_H */
