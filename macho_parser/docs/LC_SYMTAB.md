# LC_SYMTAB
In terms of programming, symbols are just the names of memory addresses. The names of class, method and global variables eventually are compiled into symbols and associated addresses. `LC_SYMTAB` is the load command that stores all the symbols of a Mach-O binary. We usually use `nm` to examine `LC_SYMTAB`.

`LC_SYMTAB`, living in `__LINKEDIT` segment, has two parts: a string table and a symbol table.

``` c
// LC_SYMTAB structure
struct symtab_command {
    uint32_t cmd;        /* LC_SYMTAB */
    uint32_t cmdsize;    /* sizeof(struct symtab_command) */
    uint32_t symoff;     /* symbol table offset */
    uint32_t nsyms;      /* number of symbol table entries */
    uint32_t stroff;     /* string table offset */
    uint32_t strsize;    /* string table size in bytes */
};
```

## String Table
Technically string table is not a table. It's simply an area where a bunch of strings live. It is exclusively used for symbols. Don't confuse it with `__cstring` section, which is part of `__TEXT`.

We can hex dump this area to see its content. `\0` is the string terminator also the divider.

```
$ ./macho_parser -c LC_SYMTAB sample.out
LC_SYMTAB            cmdsize: 24     symoff: 49640   nsyms: 41   (symsize: 656)   stroff: 50336   strsize: 648

$ xxd -s 50336 -l 648 -c 24 sample.out
0000c4a0: 2000 5f4f 424a 435f 434c 4153 535f 245f 5369 6d70 6c65 436c   ._OBJC_CLASS_$_SimpleCl
0000c4b8: 6173 7300 5f4f 424a 435f 4d45 5441 434c 4153 535f 245f 5369  ass._OBJC_METACLASS_$_Si
0000c4d0: 6d70 6c65 436c 6173 7300 5f5f 6d68 5f65 7865 6375 7465 5f68  mpleClass.__mh_execute_h
0000c4e8: 6561 6465 7200 5f63 5f63 6f6e 7374 7275 6374 6f72 5f66 756e  eader._c_constructor_fun
0000c500: 6374 696f 6e00 5f63 5f75 7365 645f 6675 6e63 7469 6f6e 005f  ction._c_used_function._
0000c518: 635f 7765 616b 5f69 6d70 6f72 745f 6675 6e63 7469 6f6e 005f  c_weak_import_function._
0000c530: 6d61 696e 005f 4e53 4c6f 6700 5f4f 424a 435f 434c 4153 535f  main._NSLog._OBJC_CLASS_
0000c548: 245f 4e53 4f62 6a65 6374 005f 4f42 4a43 5f4d 4554 4143 4c41  $_NSObject._OBJC_METACLA
0000c560: 5353 5f24 5f4e 534f 626a 6563 7400 5f5f 5f43 4643 6f6e 7374  SS_$_NSObject.___CFConst
0000c578: 616e 7453 7472 696e 6743 6c61 7373 5265 6665 7265 6e63 6500  antStringClassReference.
......
```

## Symbol Table
A symbol table contains a list of `nlist`, which has its own header file [`nlist.h`](../../apple_open_source/xnu/EXTERNAL_HEADERS/mach-o/nlist.h). Each `nlist` has a symbol name (actually an index into the string table), an address and other attributes.

```c
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

### n_strx
A `nlist` doesn't directly contain a string. Instead, it has an index pointing to the string table. This is the symbol name.

### n_value
It is just the address. Please note undefined symbol doesn't have an address, because the address is defined elsewhere.

### n_type
The format of `n_type`:
```
0000 0000
─┬─│ ─┬─│
 │ │  │ └─ N_EXT (external symbol)
 │ │  └─ N_TYPE (N_UNDF, N_ABS, N_SECT, N_PBUD, N_INDR)
 │ └─ N_PEXT (private external symbol)
 └─ N_STAB (debugging symbol)
```

### n_sect
If the symbol type is `N_SECT` (`nlist.n_type & N_TYPE`), this field is the ordinal of sections that appear in the Mach-O binary. Otherwise this field should be 0 (`NO_SECT`).

### n_desc
The format of `n_desc`:
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

##### REFERENCE_TYPE
I'm not sure how exactly the `REFERENCE_TYPE` is used. My understanding is that global variables are non-lazy bound and functions are lazily bound (see [dynamic linking](https://github.com/qyang-nj/llios/tree/main/dynamic_linking)). However, those function symbols are `REFERENCE_FLAG_UNDEFINED_NON_LAZY`. That's why I'm confused.

##### N_NO_DEAD_STRIP
Enabled by `__attribute__((constructor))`. It tells the linker (`ld`) to keep this symbol even it's not used. It exits in object files (`MH_OBJECT`). Read more about [dead code elimination](https://github.com/qyang-nj/llios/tree/main/dce).

##### N_WEAK_REF
Enabled by `__attribute__((weak))`. It tells dynamic loader (`dyld`) if the symbol cannot be found at runtime, set NULL to its address.

##### LIBRARY_ORDINAL
The index of the library where the symbols is defined. The number 1~253 (inclusive) is the ordinal of `LC_*_DYLIB` load commands that appear in the Mach-O binary. The number 254 (`DYNAMIC_LOOKUP_ORDINAL`), which is for backward compatibility, means the symbol is not two-level namespace. The number 255 (`EXECUTABLE_ORDINAL`), existing in `MH_BUNDLE`, means this symbol is defined in the executable. See below "Two Level Namespace".

## Two Level Namespace
The linker enables [the two-level namespace](http://mirror.informatimago.com/next/developer.apple.com/releasenotes/DeveloperTools/TwoLevelNamespaces.html) (`-twolevel_namespace`) by default. It can be disabled by `-flat_namespace` option. The first level of the two-level namespace is the name of the library that contains the symbol, and the second is the name of the symbol. Once enabled, the macho header has `MH_TWOLEVEL` flag set. Each undefined symbol records its library information in `LIBRARY_ORDINAL` of `nlist.n_desc`.

There are two major benefits of two-level namespace:
* avoid symbol conflict from different libraries
* accelerate symbol lookup at runtime

