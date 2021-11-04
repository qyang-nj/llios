# LC_SYMTAB
``` c
struct symtab_command {
    uint32_t cmd;        /* LC_SYMTAB */
    uint32_t cmdsize;    /* sizeof(struct symtab_command) */
    uint32_t symoff;     /* symbol table offset */
    uint32_t nsyms;      /* number of symbol table entries */
    uint32_t stroff;     /* string table offset */
    uint32_t strsize;    /* string table size in bytes */
};

struct nlist_64 {
    union {
        uint32_t n_strx;  /* index into the string table */
    } n_un;
    uint8_t n_type;       /* type flag, see below */
    uint8_t n_sect;       /* section number or NO_SECT */
    uint16_t n_desc;      /* see <mach-o/stab.h> */
    uint64_t n_value;     /* value of this symbol (or stab offset) */
};
```

Symbol table contains a list of `nlist` and a string table. Both are part of `__LINKEDIT`. The string table here are exclusively used for symbols. Don't confuse it with `__cstring` section, which is part of `__TEXT`.

### n_type
```
0000 0000
─┬─│ ─┬─│
 │ │  │ └─ N_EXT (external symbol)
 │ │  └─ N_TYPE (N_UNDF, N_ABS, N_SECT, N_PBUD, N_INDR)
 │ └─ N_PEXT (private external symbol)
 └─ N_STAB (debugging symbol)
```

### n_desc
```
0000 0000 0000 0000
────┬──── ││││  ─┬─
    │     ││││   └─ REFERENCE_TYPE (used by undefined symbols)
    │     │││└─ REFERENCED_DYNAMICALLY
    │     ││└─ NO_DEAD_STRIP
    │     │└─ N_WEAK_REF
    │     └─ N_WEAK_DEF
    └─ LIBRARY_ORDINAL (used by two-level namespace)
```

#### REFERENCE_TYPE
I'm not sure how exactly the `REFERENCE_TYPE` is used. My understanding is that global variables are non-lazy bound and functions are lazily bound (see [dynamic linking](https://github.com/qyang-nj/llios/tree/main/dynamic_linking)). However, those function symbols are `REFERENCE_FLAG_UNDEFINED_NON_LAZY`. That's why I'm confused.

#### N_NO_DEAD_STRIP
Enabled by `__attribute__((constructor))`. It tells the linker (`ld`) to keep this symbol even it's not used. It exits in object files (`MH_OBJECT`). Read more about [dead code elimination](https://github.com/qyang-nj/llios/tree/main/dce).

#### N_WEAK_REF
Enabled by `__attribute__((weak))`. It tells dynamic loader (`dyld`) if the symbol cannot be found at runtime, set NULL to its address.

# Two Level Namespace
The linker enables the two-level namespace option (`-twolevel_namespace`) by default. It can be disabled by `-flat_namespace` option. The first level of the two-level namespace is the name of the library that contains the symbol, and the second is the name of the symbol. Once enabled, the macho header has `MH_TWOLEVEL` flag set. Each undefined symbols will record its library information by `LIBRARY_ORDINAL` in `nlist.n_desc`.

Two major benefits of two-level namespace:
* avoid symbol conflict from different libraries
* accelerate symbol lookup at runtime

##### Learn more
[Mac OS X Developer Release Notes: Two-Level Namespace Executables](http://mirror.informatimago.com/next/developer.apple.com/releasenotes/DeveloperTools/TwoLevelNamespaces.html)
