# MachO Parser
To learn the MachO format, the best way is to build a parser from scratch. It helps understand how MachO format is exactly laid out.

This parser actually turns out to be a super light version of the combination of  `otool`, `nm`, `strings`, etc.

## Usage
Build and run,
``` bash
./build.sh
./parser /path/to/a/macho
```

There is a sample that you can change the code and observe how the macho is changed.
```
cd sample && ./build.sh && cd ..
./parser sample/sample
```

## Notes
### LC_SEGMENT_64
#### __mod_init_func
`(__DATA,__mod_init_func)` or `(__DATA_CONST,__mod_init_func)`

This is the section that contains of a list of function pointers, which will [be executed by `dyld`](https://github.com/opensource-apple/dyld/blob/3f928f32597888c5eac6003b9199d972d49857b5/src/ImageLoaderMachO.cpp#L1815~L1847) before `main`. Those are functions with `__attribute__((constructor))` and they will affect the app launch time.

Once we have the function pointer address, we can use `atos` to query the function name.
```
> xcrun atos -o sample/sample 0x100003f20
c_constructor_function (in sample) + 0
```

⚠️ Please note that ObjC's `+load` methods will also be executed before `main`, but uses a different mechanism. See below "+load in ObjC" section.

### LC_SYMTAB
#### Two Level Namespace
The linker enables the two-level namespace option (`-twolevel_namespace`) by default. It can be disabled by `-flat_namespace` option. The first level of the two-level namespace is the name of the library that contains the symbol, and the second is the name of the symbol. Once enabled, the macho header has `MH_TWOLEVEL` flag set. Each undefined symbols will record its library information by `LIBRARY_ORDINAL` in `nlist.n_desc`.

Two major benefits of two-level namespace:
* avoid symbol conflict from different libraries
* accelerate symbol lookup at runtime

##### References
[Mac OS X Developer Release Notes: Two-Level Namespace Executables]()

#### N_NO_DEAD_STRIP
Enabled by `__attribute__((constructor))`. It tells the linker (`ld`) to keep this symbol even it's not used. It's a flag in `nlist.n_desc` and exits in object of instead file executable.

#### N_WEAK_REF
Enabled by `__attribute__((weak))`. It tells dynamic loader (`dyld`) if the symbol cannot be found at runtime, set NULL to its address.

### LC_DYSYMTAB
This load command is used to support dynamic linking.
```
struct dysymtab_command { ... }
```

#### Indirect symbol table
```
struct dysymtab_command {
    // ...
    uint32_t indirectsymoff; /* file offset to the indirect symbol table */
    uint32_t nindirectsyms;  /* number of indirect symbol table entries */
    // ...
};
```
The indirect symbol is an array of 32-bit values. Each value is an index to symbols in `SYMTAB`. It's used to record the symbol associated to the pointer in the `__stubs`,`__got`, add `__la_symbol_ptr` sections. These sections uses `reserved1` and `reserved2` to indicate the start position and length in the indirect table.
```
struct section_64
    // ...
	uint32_t	reserved1;	/* reserved (for offset or index) */
	uint32_t	reserved2;	/* reserved (for count or sizeof) */
    // ...
};
```

For example, the symbol of the 3rd pointer in `__got` is (in pseudocode):
```
symbol_table[indirect_symbol_table[__got.section_64.reserved + (3 - 1)]]
```


We can use `otool -I` to dump the indirect symbol table.

### Other
#### `+load` in ObjC
The MachO that has Objective-C code will have two sections, `(__DATA_CONST,__objc_classlist)` and `(__DATA_CONST,__objc_nlclslist)`. `__objc_classlist` includes the addresses of all ObjC classes, while `__objc_nlclslist` contains only *non-lazy* classes. [Non-lazy classes are classes that have `+load` method](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-runtime-new.mm#L2806~L2812) and will be loaded at launch time.

**How `+load` is executed during startup?**
1. dyld calls [_objc_init](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-os.mm#L803~L831), where [a notification is registered when an image is loaded](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-os.mm#L830).
2. In the notification callback, [load_images](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-runtime-new.mm#L2157~L2193), it [calls the load methods](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-loadmethod.mm#L306~L365) in that image.

The difference between `+load` and `__mod_init_func` is that the former guarantees [certain order of execution](https://developer.apple.com/documentation/objectivec/nsobject/1418815-load?language=objc), while the latter doesn't.

##### References
* [Objective-C: What is a lazy class?](https://stackoverflow.com/a/15318325/3056242)
* [What did Runtime do during the startup of Runtime objc4-779.1 App?](https://programmer.group/what-did-runtime-do-during-the-startup-of-runtime-objc4-779.1-app.html)
