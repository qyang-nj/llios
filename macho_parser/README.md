# Mach-O Format
This is **not** a complete reference of Mach-O format. It's more like my personal study note. The content will be growing as I'm learning.

## Mach-O Parser
To learn the Mach-O format, the best way is to build a parser from scratch. It helps me understand, byte by byte, how Mach-O format is exactly laid out. This parser actually turns out to be a super light version of the combination of  `otool`, `nm`, `strings`, etc. To build and run,
``` bash
./build.sh
./parser /path/to/a/macho
```

This directory also has a sample that you can change the code and observe how the macho is changed.
```
cd sample && ./build.sh && cd ..
./parser sample/sample
```

## LC_SEGMENT_64
#### __mod_init_func
`(__DATA,__mod_init_func)` or `(__DATA_CONST,__mod_init_func)`

This is the section that contains of a list of function pointers, which will [be executed by `dyld`](https://github.com/opensource-apple/dyld/blob/3f928f32597888c5eac6003b9199d972d49857b5/src/ImageLoaderMachO.cpp#L1815~L1847) before `main`. Those are functions with `__attribute__((constructor))` and they will affect the app launch time.

Once we have the function pointer address, we can use `atos` to query the function name.
```
> xcrun atos -o sample/sample 0x100003f20
c_constructor_function (in sample) + 0
```

⚠️ Please note that ObjC's `+load` methods will also be executed before `main`, but uses a different mechanism. See below "+load in ObjC" section.

## LC_SYMTAB
``` c
/* This is the symbol table entry structure for 64-bit architectures. */
struct nlist_64 {
    union {
        uint32_t  n_strx;  /* index into the string table */
    } n_un;
    uint8_t n_type;        /* type flag, see below */
    uint8_t n_sect;        /* section number or NO_SECT */
    uint16_t n_desc;       /* see <mach-o/stab.h> */
    uint64_t n_value;      /* value of this symbol (or stab offset) */
};
```

### n_desc
`n_desc` is a field of `nlist_64`. Although it's only 16 bits, it's packed a lot of information about a symbol.
```
0000 0000 0000 0000
────┬──── ││││  ─┬─
    │     ││││   └─ REFERENCE_TYPE
    │     │││└─ REFERENCED_DYNAMICALLY
    │     ││└─ NO_DEAD_STRIP
    │     │└─ N_WEAK_REF
    │     └─ N_WEAK_DEF
    └─ LIBRARY_ORDINAL (used by two-level namespace)
```

#### N_NO_DEAD_STRIP
Enabled by `__attribute__((constructor))`. It tells the linker (`ld`) to keep this symbol even it's not used. It exits in object files (`MH_OBJECT`). Read more about [dead code elimination](https://github.com/qyang-nj/llios/tree/main/dce).

#### N_WEAK_REF
Enabled by `__attribute__((weak))`. It tells dynamic loader (`dyld`) if the symbol cannot be found at runtime, set NULL to its address.

### Two Level Namespace
The linker enables the two-level namespace option (`-twolevel_namespace`) by default. It can be disabled by `-flat_namespace` option. The first level of the two-level namespace is the name of the library that contains the symbol, and the second is the name of the symbol. Once enabled, the macho header has `MH_TWOLEVEL` flag set. Each undefined symbols will record its library information by `LIBRARY_ORDINAL` in `nlist.n_desc`.

Two major benefits of two-level namespace:
* avoid symbol conflict from different libraries
* accelerate symbol lookup at runtime

##### Learn more
[Mac OS X Developer Release Notes: Two-Level Namespace Executables](http://mirror.informatimago.com/next/developer.apple.com/releasenotes/DeveloperTools/TwoLevelNamespaces.html)

## LC_DYSYMTAB
This load command is used to support dynamic linking.
```
struct dysymtab_command { ... }
```

### Indirect symbol table
```
struct dysymtab_command {
    // ...
    uint32_t indirectsymoff; /* file offset to the indirect symbol table */
    uint32_t nindirectsyms;  /* number of indirect symbol table entries */
    // ...
};
```
The indirect symbol is an array of 32-bit values. Each value is an index to symbols in `SYMTAB`. It's used to record the symbol associated to the pointer in the `__stubs`,`__got` add `__la_symbol_ptr` sections. These sections uses `reserved1` to indicate the start position in the indirect table. The length usually is `struct section_64.size / sizeof(uintptr_t)`.
```
struct section_64
    // ...
    uint32_t reserved1; /* reserved (for offset or index) */
    // ...
};
```

For example, the symbol of the 3rd pointer in `__got` is (in pseudocode):
```
symbol_table[indirect_symbol_table[__got.section_64.reserved + (3 - 1)]]
```

In practice, We use `otool -I` to dump the indirect symbol table.
```
$ otool -I a.out
a.out:
Indirect symbols for (__TEXT,__stubs) 1 entries
address            index
0x0000000100003f96     3                                  --> _lib_func
Indirect symbols for (__DATA_CONST,__got) 2 entries
address            index
0x0000000100004000     4                                  --> _lib_str
0x0000000100004008     5                                  --> dyld_stub_binder
Indirect symbols for (__DATA,__la_symbol_ptr) 1 entries
address            index
0x0000000100008000     3                                  --> _lib_func
```

Then we can look up the indices through `nm` to find out the actual symbols.
```
$ nm -ap a.out | nl -v 0
     0	0000000100008008 d __dyld_private
     1	0000000100000000 T __mh_execute_header
     2	0000000100003f70 T _main
     3	                 U _lib_func
     4	                 U _lib_str
     5	                 U dyld_stub_binder
```
(The `-a` and `-p` for `nm` are really important here. They make sure the all symbols are listed in the same order as in `SYMTAB`.)

## LC_LINKER_OPTION

`LC_LINKER_OPTION` only exists in the object files (`MH_OBJECT`) and is used for auto-linking. This load command literally contains linker flags that will be used by the static linker.

##### Learn more
[Auto Linking on iOS & macOS](https://milen.me/writings/auto-linking-on-ios-and-macos/)

## Other
### `+load` in ObjC
The MachO that has Objective-C code will have two sections, `(__DATA_CONST,__objc_classlist)` and `(__DATA_CONST,__objc_nlclslist)`. `__objc_classlist` includes the addresses of all ObjC classes, while `__objc_nlclslist` contains only *non-lazy* classes. [Non-lazy classes are classes that have `+load` method](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-runtime-new.mm#L2806~L2812) and will be loaded at launch time.

**How `+load` is executed during startup?**
1. dyld calls [_objc_init](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-os.mm#L803~L831), where [a notification is registered when an image is loaded](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-os.mm#L830).
2. In the notification callback, [load_images](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-runtime-new.mm#L2157~L2193), it [calls the load methods](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-loadmethod.mm#L306~L365) in that image.

The difference between `+load` and `__mod_init_func` is that the former guarantees [certain order of execution](https://developer.apple.com/documentation/objectivec/nsobject/1418815-load?language=objc), while the latter doesn't.

##### Learn more
* [Objective-C: What is a lazy class?](https://stackoverflow.com/a/15318325/3056242)
* [What did Runtime do during the startup of Runtime objc4-779.1 App?](https://programmer.group/what-did-runtime-do-during-the-startup-of-runtime-objc4-779.1-app.html)
