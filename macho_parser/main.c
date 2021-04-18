#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

void parse_load_commands(FILE *fptr, int offset, uint32_t ncmds);
void parse_sections(FILE *fptr, struct segment_command_64 *seg_cmd);
void parse_cstring_section(FILE *fptr, struct section_64 *cstring_sect);
void parse_mod_init_func_section(FILE *fptr, struct section_64 *sect);
void parse_symbol_table(FILE *fptr, struct symtab_command *sym_cmd);
void format_string(char *str, char *formatted);


void *load_bytes(FILE *fptr, int offset, int size) {
    void *buf = calloc(1, size);
    fseek(fptr, offset, SEEK_SET);
    fread(buf, size, 1, fptr);
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        puts("Error: missing Mach-O file.");
        return 1;
    }
    FILE *fptr = fopen(argv[1], "rb");

    struct mach_header_64 *header = load_bytes(fptr, 0, sizeof(struct mach_header_64));
    parse_load_commands(fptr, sizeof(struct mach_header_64), header->ncmds);

    free(header);
    fclose(fptr);
    return 0;
}

void parse_load_commands(FILE *fptr, int offset, uint32_t ncmds)
{
    for (int i = 0; i < ncmds; ++i)
    {
        struct load_command *lcmd = load_bytes(fptr, offset, sizeof(struct load_command));

        if (lcmd->cmd == LC_SEGMENT_64)
        {
            struct segment_command_64 *seg_cmd = load_bytes(fptr, offset, lcmd->cmdsize);
            printf("LC_SEGMENT_64: %s (%lld)\n", seg_cmd->segname, seg_cmd->filesize);
            parse_sections(fptr, seg_cmd);
            free(seg_cmd);
        }
        else if (lcmd->cmd == LC_SYMTAB)
        {
            struct symtab_command *cmd = load_bytes(fptr, offset, lcmd->cmdsize);
            printf("LC_SYMTAB (symtab: %lu, strtab: %u)\n", cmd->nsyms * sizeof(struct nlist_64), cmd->strsize);
            parse_symbol_table(fptr, cmd);
            free(cmd);
        }

        offset += lcmd->cmdsize;
        free(lcmd);
    }
}

void parse_sections(FILE *fptr, struct segment_command_64 *seg_cmd)
{
    // section_64 is immediately after segment_command_64.
    struct section_64 *sections = (void *)seg_cmd + sizeof(struct segment_command_64);

    for (int i = 0; i < seg_cmd->nsects; ++i)
    {
        struct section_64 sect = sections[i];
        printf("    (%s,%s) [size: %lld]\n", sect.segname, sect.sectname, sect.size);

        const uint8_t type = sect.flags & SECTION_TYPE;

        // (__TEXT,__cstring), (__TEXT,__objc_classname__TEXT), (__TEXT,__objc_methname), etc..
        if (type == S_CSTRING_LITERALS) // __cstring
        {
            parse_cstring_section(fptr, &sect);
        }
        // (__DATA_CONST,__mod_init_func)
        else if (type == S_MOD_INIT_FUNC_POINTERS)
        {
            parse_mod_init_func_section(fptr, &sect);
        }
    }
}

void parse_cstring_section(FILE *fptr, struct section_64 *cstring_sect)
{
    void *section = load_bytes(fptr, cstring_sect->offset, cstring_sect->size);

    char formatted[256];
    for (char *ptr = section; ptr < (char *)(section + cstring_sect->size);)
    {
        if (strlen(ptr) > 0) {
            format_string(ptr, formatted);
            printf("        \"%s\"\n", formatted);
            ptr += strlen(ptr);
        }
        ptr += 1;
    }

    free(section);
}

void parse_mod_init_func_section(FILE *fptr, struct section_64 *sect)
{
    void *section = load_bytes(fptr, sect->offset, sect->size);

    const size_t count = sect->size / sizeof(uintptr_t);
    for (int i = 0; i < count; ++i) {
        printf("        0x%lx\n", *((uintptr_t *)section + i));
    }

    free(section);
}

void parse_symbol_table(FILE *fptr, struct symtab_command *sym_cmd)
{
    void *sym_table = load_bytes(fptr, sym_cmd->symoff, sym_cmd->nsyms * sizeof(struct nlist_64));
    void *str_table = load_bytes(fptr, sym_cmd->stroff, sym_cmd->strsize);

    for (int i = 0; i < sym_cmd->nsyms; ++i)
    {
        struct nlist_64 *nlist = sym_table + sizeof(struct nlist_64) * i;
        char *symbol = str_table + nlist->n_un.n_strx;
        char *no_dead_strip = (nlist->n_desc & N_NO_DEAD_STRIP) ? " [no dead strip]" : "";

        if (strlen(symbol) > 0) {
            printf("        0x%016llx  %s%s\n", nlist->n_value, symbol, no_dead_strip);
        }
    }

    free(sym_table);
    free(str_table);
}

// If the string contains '\n', replace with literal "\n".s
void format_string(char *str, char *formatted)
{
    int j = 0;
    for (int i = 0; str[i] != '\0'; ++i)
    {
        switch(str[i])
        {
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
