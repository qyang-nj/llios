# Mach-O Parser
To learn the Mach-O format, no way is better than building a parser from scratch. This helps me understand, byte by byte, how Mach-O format is laid out. This parser actually turns out to be a super light version of the combination of  `otool`, `nm`, `strings`, `codesign` etc.

#### Usage
To build the parser, run `./build.sh --openssl`. (OpenSSL is not required if not parsing code signature.)

```
$ ./macho_parser --help
Usage: macho_parser [options] macho_file
    -c, --command LOAD_COMMAND           show specific load command
    -v, --verbose                        can be used multiple times to increase verbose level
        --no-truncate                    do not truncate even the content is long
    -h, --help                           show this help message

    --build-version                      equivalent to '--comand LC_BUILD_VERSION --comand LC_VERSION_MIN_*'

Code Signature Options:
    --cs,  --code-signature              equivalent to '--command LC_CODE_SIGNATURE'
    --cd,  --code-directory              show Code Directory
    --ent, --entitlement                 show the embedded entitlement
           --blob-wrapper                show the blob wrapper (signature blob)

Dynamic Symbol Table Options:
    --dysymtab                           equivalent to '--command LC_DYSYMTAB'
    --local                              show local symbols
    --extdef                             show externally (public) defined symbols
    --undef                              show undefined symbols
    --indirect                           show indirect symbol table
```

#### Sample
This directory also includes a sample that demonstrates how source code ends up in the different parts of a binary file.

```
$ ./build_sample.sh
$ ./macho_parser sample.out
LC_SEGMENT_64        cmdsize: 72     segname: __PAGEZERO       fileoff: 0x00000000 filesize: 0            (fileend: 0x00000000)
LC_SEGMENT_64        cmdsize: 712    segname: __TEXT           fileoff: 0x00000000 filesize: 16384        (fileend: 0x00004000)
LC_SEGMENT_64        cmdsize: 552    segname: __DATA_CONST     fileoff: 0x00004000 filesize: 16384        (fileend: 0x00008000)
LC_SEGMENT_64        cmdsize: 392    segname: __DATA           fileoff: 0x00008000 filesize: 16384        (fileend: 0x0000c000)
LC_SEGMENT_64        cmdsize: 72     segname: __LINKEDIT       fileoff: 0x0000c000 filesize: 1328         (fileend: 0x0000c530)
LC_SYMTAB            cmdsize: 24     symoff: 0x8c360   nsyms: 21   (symsize: 336)   stroff: 0x08c360   strsize: 464
LC_DYSYMTAB          cmdsize: 80     nlocalsym: 5  nextdefsym: 7   nundefsym: 9   nindirectsyms: 10
LC_LOAD_DYLIB        cmdsize: 48     @rpath/my_dylib.dylib
LC_LOAD_DYLIB        cmdsize: 56     /usr/lib/libSystem.B.dylib
LC_LOAD_DYLIB        cmdsize: 104    /System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation
LC_LOAD_DYLIB        cmdsize: 96     /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation
LC_LOAD_DYLIB        cmdsize: 56     /usr/lib/libobjc.A.dylib
LC_RPATH             cmdsize: 24     build
```

## LC_SEGMENT_64
``` c
struct segment_command_64 {     /* for 64-bit architectures */
    uint32_t    cmd;            /* LC_SEGMENT_64 */
    uint32_t    cmdsize;        /* includes sizeof section_64 structs */
    char        segname[16];    /* segment name */
    uint64_t    vmaddr;         /* memory address of this segment */
    uint64_t    vmsize;         /* memory size of this segment */
    uint64_t    fileoff;        /* file offset of this segment */
    uint64_t    filesize;       /* amount to map from the file */
    vm_prot_t   maxprot;        /* maximum VM protection */
    vm_prot_t   initprot;       /* initial VM protection */
    uint32_t    nsects;         /* number of sections in segment */
    uint32_t    flags;          /* flags */
};
```
A `LC_SEGMENT_64` defines a segment, which basically is a chunk of continuous space that will be `mmap`'d to the memory. Most segments are well-known, like `__TEXT` and `__DATA`. Besides their names, the key different among segments is the memory protection mode (`initprot` and `maxprot`).

### __PAGEZERO
`__PAGEZERO` segment has zero size on disk but 4GB in VM. Its main purpose is to trap NULL dereference, causing segment fault.

### __TEXT
`__TEXT` segment is where the executable code is. Thus it's readable (`VM_PROT_READ`) and executable (`VM_PROT_EXECUTE`), but not writable (`VM_PROT_WRITE`).

### __DATA
`__DATA` segment is readable and writable, which makes it useful for mutable data. Fox example, lay binding (`__la_symbol_ptr`).

#### __la_symbol_ptr
Lazy Symbol Pointer. It's used for lazy binding during [dynamic linking](../dynamic_linking).

### __DATA_CONST
`__DATA_CONST` segment stores constant data, some of which needs to be initialized. At the time of `mmap`, `__DATA_CONST`, same as `__DATA`, is readable and writable. Once initialized, `dyld` will change this segment to just readable via `mprotect`. Then it becomes real constant. One of use cases for this is the non-lazy biding (`__got`).

#### __got
Global Offset Table. See [dynamic linking](../dynamic_linking).

#### __mod_init_func
This is the section that contains of a list of function pointers, which will [be executed by `dyld`](https://github.com/opensource-apple/dyld/blob/3f928f32597888c5eac6003b9199d972d49857b5/src/ImageLoaderMachO.cpp#L1815~L1847) before `main`. Those are functions with `__attribute__((constructor))` and they will affect the app launch time.

Once we have the function pointer address, we can use `atos` to query the function name.
```
> xcrun atos -o sample/sample 0x100003f20
c_constructor_function (in sample) + 0
```

⚠️ Please note that ObjC's `+load` methods will also be executed before `main`, but uses a different mechanism. See below "+load in ObjC" section.

## __LINKEDIT
`__LINKEDIT` segment contains data that's used by the linker. Unlike other segments, this one doesn't have sections. Its contents are described by other load commands.

## LC_DYLD_INFO_ONLY
``` c
struct dyld_info_command {
    uint32_t cmd;            /* LC_DYLD_INFO or LC_DYLD_INFO_ONLY */
    uint32_t cmdsize;        /* sizeof(struct dyld_info_command) */

    uint32_t rebase_off;     /* file offset to rebase info  */
    uint32_t rebase_size;    /* size of rebase info   */

    uint32_t bind_off;       /* file offset to binding info   */
    uint32_t bind_size;      /* size of binding info  */

    uint32_t weak_bind_off;  /* file offset to weak binding info   */
    uint32_t weak_bind_size; /* size of weak binding info  */

    uint32_t lazy_bind_off;  /* file offset to lazy binding info */
    uint32_t lazy_bind_size; /* size of lazy binding infs */

    uint32_t export_off;     /* file offset to lazy binding info */
    uint32_t export_size;    /* size of lazy binding infs */
};
```

This load command is only used by `dyld` at runtime. The information here can inspected by `xcrun dyldinfo (-rebase|-bind|-weak_bind|-lazy_bind|-export)`.

### Export Info
A deep dive of exported info is at "[exported_symbol](../exported_symbol)".

## LC_SYMTAB
```
$ ./macho_parser --command LC_SYMTAB --no-truncate sample.out
LC_SYMTAB            cmdsize: 24     symoff: 49640   nsyms: 41   (symsize: 656)   stroff: 50336   strsize: 648
    0   : 0000000100003f00  [N_SECT]  +[SimpleClass load]
    1   : 0000000100008020  [N_SECT]  __OBJC_$_CLASS_METHODS_SimpleClass
    2   : 0000000100008040  [N_SECT]  __OBJC_METACLASS_RO_$_SimpleClass
    3   : 0000000100008088  [N_SECT]  __OBJC_CLASS_RO_$_SimpleClass
    4   : 0000000100008120  [N_SECT]  __dyld_private
    5   : 0000000000000000  [N_STAB(0x60)]  /Users/qingyang/Projects/llios/macho_parser/sample/
    ...
    36  :                   [N_EXT N_UNDF]  __objc_empty_cache  [UNDEFINED_NON_LAZY LIBRARY_ORDINAL(5)]
    37  :                   [N_EXT N_UNDF]  _c_extern_weak_function  [UNDEFINED_NON_LAZY N_WEAK_REF LIBRARY_ORDINAL(254)]
    38  :                   [N_EXT N_UNDF]  _my_dylib_func  [UNDEFINED_NON_LAZY LIBRARY_ORDINAL(1)]
    39  :                   [N_EXT N_UNDF]  _printf  [UNDEFINED_NON_LAZY LIBRARY_ORDINAL(2)]
    40  :                   [N_EXT N_UNDF]  dyld_stub_binder  [UNDEFINED_NON_LAZY LIBRARY_ORDINAL(2)]
```

The details of `LC_SYMTAB` is [here](docs/LC_SYMTAB.md).

## LC_DYSYMTAB
```
./macho_parser --dysymtab --local --extdef --undef --indirect sample.out
LC_DYSYMTAB          cmdsize: 80     nlocalsym: 25  nextdefsym: 7   nundefsym: 9   nindirectsyms: 10
  ilocalsym     : 0           nlocalsym    : 25
  iextdefsym    : 25          nextdefsym   : 7
  iundefsym     : 32          nundefsym    : 9
  tocoff        : 0x0         ntoc         : 0
  modtaboff     : 0x0         nmodtab      : 0
  extrefsymoff  : 0x0         nextrefsyms  : 0
  indirectsymoff: 0x0000c478  nindirectsyms: 10
  extreloff     : 0x0         nextrel      : 0
  locreloff     : 0x0         nlocrel      : 0

  Local symbols (ilocalsym 0, nlocalsym:25)
    0   : 0000000100003f00  [N_SECT]  +[SimpleClass load]
    1   : 0000000100008020  [N_SECT]  __OBJC_$_CLASS_METHODS_SimpleClass
    2   : 0000000100008040  [N_SECT]  __OBJC_METACLASS_RO_$_SimpleClass
    ...

  Externally defined symbols (iextdefsym: 25, nextdefsym:7)
    25  : 00000001000080f8  [N_EXT N_SECT]  _OBJC_CLASS_$_SimpleClass
    26  : 00000001000080d0  [N_EXT N_SECT]  _OBJC_METACLASS_$_SimpleClass
    27  : 0000000100000000  [N_EXT N_SECT]  __mh_execute_header  [REFERENCED_DYNAMICALLY]
    ...

  Undefined symbols (iundefsym: 32, nundefsym:9)
    32  : [N_EXT N_UNDF]  _NSLog  [UNDEFINED_NON_LAZY LIBRARY_ORDINAL(4)]
    33  : [N_EXT N_UNDF]  _OBJC_CLASS_$_NSObject  [UNDEFINED_NON_LAZY LIBRARY_ORDINAL(5)]
    34  : [N_EXT N_UNDF]  _OBJC_METACLASS_$_NSObject  [UNDEFINED_NON_LAZY LIBRARY_ORDINAL(5)]
    ...

  Indirect symbol table (indirectsymoff: 0xc478, nindirectsyms: 10)
    0  -> 32  : [N_EXT N_UNDF]  _NSLog  [UNDEFINED_NON_LAZY LIBRARY_ORDINAL(4)]
    1  -> 37  : [N_EXT N_UNDF]  _c_extern_weak_function  [UNDEFINED_NON_LAZY N_WEAK_REF LIBRARY_ORDINAL(254)]
    2  -> 38  : [N_EXT N_UNDF]  _my_dylib_func  [UNDEFINED_NON_LAZY LIBRARY_ORDINAL(1)]
    ...
```

The details of `LC_SYMTAB` is [here](docs/LC_DYSYMTAB.md).

## LC_FUNCTION_STARTS
This load command indicates a list of all fucntion addresses, which are encoded by a list of [ULEB128](https://en.wikipedia.org/wiki/LEB128) numbers. The first number is the first function's offset to `__TEXT`'s `vmaddr`. The following numbers are the offset to the previous address. (Detailed information can be found in the [`dyldinfo` source code](https://github.com/qyang-nj/llios/blob/49f0fab2f74f0ecb03ee9ae1f54953bc9ad86384/apple_open_source/ld64/src/other/dyldinfo.cpp#L2045-L2071)). In the example of our sample program, here is its `LC_FUNCTION_STARTS`.
```
$ otool -l sample.out | grep LC_FUNCTION_STARTS -A3
      cmd LC_FUNCTION_STARTS
  cmdsize 16
  dataoff 49632
 datasize 8

$ xxd -s 49632 -l 8 sample.out
0000c1e0: f07c 1010 1060 0000
```
These are a sequence of ULEB128-encoded numbers: 0x3e70, 0x10, 0x10, 0x10, 0x60. Since `vmaddr` of `__TEXT` is 0x100000000, the function addresses are
```
0x100003E70   (0x100000000 + 0x3e70)
0x100003E80   (0x100003E70 + 0x10)
0x100003E90   (0x100003E80 + 0x10)
0x100003EA0   (0x100003EA0 + 0x10)
0x100003F00   (0x100003E80 + 0x60)
```

The function starts are only addresses, no function names. We can use `dyldinfo -function_starts` to dump the function addresses, which will also look up symbol table to get function names. To strip this section, pass `-no_function_starts` to `ld`.

##### Learn more
[Symbolication: Beyond the basics](https://developer.apple.com/videos/play/wwdc2021/10211/), starting at 14:22.

## LC_MAIN
``` c
struct entry_point_command {
    uint32_t  cmd;	/* LC_MAIN only used in MH_EXECUTE filetypes */
    uint32_t  cmdsize;	/* 24 */
    uint64_t  entryoff;	/* file (__TEXT) offset of main() */
    uint64_t  stacksize;/* if not zero, initial stack size */
};
```

## LC_LINKER_OPTION
`LC_LINKER_OPTION` only exists in the object files (`MH_OBJECT`) and is used for auto-linking. This load command literally contains linker flags that will be used by the static linker.

##### Learn more
[Auto Linking on iOS & macOS](https://milen.me/writings/auto-linking-on-ios-and-macos/)

## LC_ID_DYLIB / LC_LOAD_DYLIB / LC_LOAD_WEAK_DYLIB
``` c
struct dylib_command {
    uint32_t     cmd;       /* LC_ID_DYLIB, LC_LOAD_{,WEAK_}DYLIB, LC_REEXPORT_DYLIB */
    uint32_t     cmdsize;   /* includes pathname string */
    struct dylib dylib;     /* the library identification */
};

struct dylib {
    union lc_str  name;             /* library's path name */
    uint32_t timestamp;             /* library's build time stamp */
    uint32_t current_version;       /* library's current version number */
    uint32_t compatibility_version; /* library's compatibility vers number*/
};
```

All these `LC_*_DYLIB` commands use the same `dylib_command` struct. `LC_ID_DYLIB` exists in the dynamic library (`MH_DYLIB`), and `LC_LOAD_DYLIB` and `LC_LOAD_WEAK_DYLIB` are in the binary (could be any macho-o type) that links dynamic libraries.

### Install name
The most important field of `struct dylib` is `name`, library's path name, aka, install name. An install name is just a filepath embedded within a dynamic library which tells the linker where that library can be found at runtime. To specify the install name, we pass `-install_name <path>` to the static linker at build time. For an existing library, we can use `install_name_tool` to change it.

It's worth noting that install name is a property of the dylib itself, so all the binaries linked against the same dylib contain the same install name. One way to work around this is to use `@rpath`, which let the dylib user to customize the search path (see `LC_RPATH` section below).

##### Learn more
[Linking and Install Names](https://www.mikeash.com/pyblog/friday-qa-2009-11-06-linking-and-install-names.html)

## LC_RPATH
``` c
struct rpath_command {
    uint32_t	 cmd;		/* LC_RPATH */
    uint32_t	 cmdsize;	/* includes string */
    union lc_str path;		/* path to add to run path */
};
```

The only meaningful thing that `LC_RPATH` has is a filepath. Dynamic linker will use that to replace `@rpath` in the dylib install name to search the dylib file. We can pass multiple `-rpath <path>` to the static linker, and each one results in a `LC_RPATH` is the final binary.

## LC_BUILD_VERSION
```
$ ./macho_parser --build-version sample.out
LC_BUILD_VERSION     cmdsize: 32     platform: MACOS   minos: 12.0.0   sdk: 12.0.0
    tool:  LD   version: 711.0.0
```
The details of `LC_BUILD_VERSION` is [here](docs/LC_BUILD_VERSION.md).

## LC_CODE_SIGNATURE
> The format of `LC_CODE_SIGNATURE` is described [here](docs/LC_CODE_SIGNATURE.md).

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
