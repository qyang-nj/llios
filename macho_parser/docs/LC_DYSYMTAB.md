# LC_DYSYMTAB
`LC_DYSYMTAB` load command is used to support dynamic linking. `LC_SYMTAB` must be present when `LC_DYSYMTAB` is present.
The best documentation is at `loader.h` ([mach-o/loader.h](../../apple_open_source/xnu/EXTERNAL_HEADERS/mach-o/loader.h#L878-L1031))

``` c
struct dysymtab_command {
    uint32_t cmd;           /* LC_DYSYMTAB */
    uint32_t cmdsize;       /* sizeof(struct dysymtab_command) */

    uint32_t ilocalsym;	    /* index to local symbols */
    uint32_t nlocalsym;	    /* number of local symbols */

    uint32_t iextdefsym;    /* index to externally defined symbols */
    uint32_t nextdefsym;    /* number of externally defined symbols */

    uint32_t iundefsym;	    /* index to undefined symbols */
    uint32_t nundefsym;	    /* number of undefined symbols */

    uint32_t tocoff;	    /* file offset to table of contents */
    uint32_t ntoc;          /* number of entries in table of contents */

    uint32_t modtaboff;	    /* file offset to module table */
    uint32_t nmodtab;	    /* number of module table entries */

    uint32_t extrefsymoff;  /* offset to referenced symbol table */
    uint32_t nextrefsyms;   /* number of referenced symbol table entries */

    uint32_t indirectsymoff; /* file offset to the indirect symbol table */
    uint32_t nindirectsyms;  /* number of indirect symbol table entries */

    uint32_t extreloff;	    /* offset to external relocation entries */
    uint32_t nextrel;	    /* number of external relocation entries */

    uint32_t locreloff;	    /* offset to local relocation entries */
    uint32_t nlocrel;	    /* number of local relocation entries */
};
```

## Local symbols
> The local symbols are used only for debugging.  The dynamic binding process may have to use them to indicate to the debugger the local symbols for a module that is being bound.

`ilocalsym` is the first index in the symbol table. From this structure, we can tell that **local symbols are consecutively listed in the symbol table**. So are external and undefined symbols.

## Externally defined symbols
Externally defined symbols are public symbols, which are visible to other binaries.

## Undefined symbols
Undefined symbols from other libraries. They need to be bound at dynamic loading.

## Indirect symbol table
The indirect symbol is an array of 32-bit values. Each value is an index to symbols in `SYMTAB`. It's used to record the symbol associated to the pointer in the `__stubs`,`__got` add `__la_symbol_ptr` sections. These sections uses `reserved1` to indicate the start position in the indirect table. The length usually is `struct section_64.size / sizeof(uintptr_t)`.
``` c
struct section_64
    // ...
    uint32_t reserved1; /* reserved (for offset or index) */
    // ...
};
```

For example, the symbol of the 3rd pointer in `__got` is (in pseudocode):
```
symbol_table[indirect_symbol_table[__got.section_64.reserved1 + (3 - 1)]]
```

In practice, we can use `otool -Iv` to dump the indirect symbol table.
```
$ otool -Iv sample.out
sample.out:
Indirect symbols for (__TEXT,__stubs) 4 entries
address            index name
0x0000000100003f28    32 _NSLog
0x0000000100003f2e    37 _c_extern_weak_function
0x0000000100003f34    38 _my_dylib_func
0x0000000100003f3a    39 _printf
Indirect symbols for (__DATA_CONST,__got) 2 entries
address            index name
0x0000000100004000    37 _c_extern_weak_function
0x0000000100004008    40 dyld_stub_binder
Indirect symbols for (__DATA,__la_symbol_ptr) 4 entries
address            index name
0x0000000100008000    32 _NSLog
0x0000000100008008    37 _c_extern_weak_function
0x0000000100008010    38 _my_dylib_func
0x0000000100008018    39 _printf
```

**INDIRECT_SYMBOL_LOCAL**
There are two special values in the indirect symbol table (`INDIRECT_SYMBOL_LOCAL` and `INDIRECT_SYMBOL_ABS`). It seems there is a way to have an indirect symbol for a local defined symbols. As the index is a special value, it's not pointing to any symbol in symbol table. *I'm not sure how and why a local defined symbol needs indirect symbol table.*
