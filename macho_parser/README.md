# Mach-O Parser
To learn the Mach-O format, no way is better than building a parser from scratch. This helps me understand, byte by byte, how Mach-O format is laid out. This parser actually turns out to be a super light version of the combination of  `otool`, `nm`, `strings`, `dyldinfo`, `codesign` etc.

#### Usage
To build the parser, run `./build.sh --openssl`. (OpenSSL is not required if not parsing code signature.)

```
$ ./macho_parser --help
Usage: macho_parser [options] macho_file
    -c, --command LOAD_COMMAND           show specific load command
    -v, --verbose                        can be used multiple times to increase verbose level
        --no-truncate                    do not truncate even the content is long
    -h, --help                           show this help message

    --segments                           equivalent to '--comand LC_SEGMENT_64
    --section INDEX                      show the section at INDEX
    --dylibs                             show dylib related commands
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

Dyld Info Options:
    --dyld-info                          equivalent to '--command LC_DYLD_INFO(_ONLY)'
    --rebase                             show rebase info
    --bind                               show binding info
    --weak-bind                          show weak binding info
    --lazy-bind                          show lazy binding info
    --export                             show export trie
    --opcode                             show the raw opcode instead of a table
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

## [LC_SEGMENT_64](docs/LC_SEGMENT_64.md)

```
$ ./macho_parser --segments sample.out
LC_SEGMENT_64        cmdsize: 72     segname: __PAGEZERO     file: 0x00000000-0x00000000 0B         vm: 0x000000000-0x100000000 4.00GB    prot: 0/0
LC_SEGMENT_64        cmdsize: 712    segname: __TEXT         file: 0x00000000-0x00004000 16.00KB    vm: 0x100000000-0x100004000 16.00KB   prot: 5/5
   0: 0x100003e70-0x100003f27 183B        (__TEXT,__text)                   type: S_REGULAR  offset: 15984
   1: 0x100003f28-0x100003f40 24B         (__TEXT,__stubs)                  type: S_SYMBOL_STUBS  offset: 16168   reserved1:  6
   2: 0x100003f40-0x100003f7a 58B         (__TEXT,__stub_helper)            type: S_REGULAR  offset: 16192
   3: 0x100003f7a-0x100003f9b 33B         (__TEXT,__cstring)                type: S_CSTRING_LITERALS  offset: 16250
   4: 0x100003f9b-0x100003fa7 12B         (__TEXT,__objc_classname)         type: S_CSTRING_LITERALS  offset: 16283
   5: 0x100003fa7-0x100003fac 5B          (__TEXT,__objc_methname)          type: S_CSTRING_LITERALS  offset: 16295
   6: 0x100003fac-0x100003fb4 8B          (__TEXT,__objc_methtype)          type: S_CSTRING_LITERALS  offset: 16300
   7: 0x100003fb4-0x100003ffc 72B         (__TEXT,__unwind_info)            type: S_REGULAR  offset: 16308
LC_SEGMENT_64        cmdsize: 552    segname: __DATA_CONST   file: 0x00004000-0x00008000 16.00KB    vm: 0x100004000-0x100008000 16.00KB   prot: 3/3
   8: 0x100004000-0x100004010 16B         (__DATA_CONST,__got)              type: S_NON_LAZY_SYMBOL_POINTERS  offset: 16384   reserved1:  4
   9: 0x100004010-0x100004018 8B          (__DATA_CONST,__mod_init_func)    type: S_MOD_INIT_FUNC_POINTERS  offset: 16400
  10: 0x100004018-0x100004038 32B         (__DATA_CONST,__cfstring)         type: S_REGULAR  offset: 16408
  11: 0x100004038-0x100004040 8B          (__DATA_CONST,__objc_classlist)   type: S_REGULAR  offset: 16440
  12: 0x100004040-0x100004048 8B          (__DATA_CONST,__objc_nlclslist)   type: S_REGULAR  offset: 16448
  13: 0x100004048-0x100004050 8B          (__DATA_CONST,__objc_imageinfo)   type: S_REGULAR  offset: 16456
LC_SEGMENT_64        cmdsize: 392    segname: __DATA         file: 0x00008000-0x0000c000 16.00KB    vm: 0x100008000-0x10000c000 16.00KB   prot: 3/3
  14: 0x100008000-0x100008020 32B         (__DATA,__la_symbol_ptr)          type: S_LAZY_SYMBOL_POINTERS  offset: 32768   reserved1:  6
  15: 0x100008020-0x1000080d0 176B        (__DATA,__objc_const)             type: S_REGULAR  offset: 32800
  16: 0x1000080d0-0x100008120 80B         (__DATA,__objc_data)              type: S_REGULAR  offset: 32976
  17: 0x100008120-0x100008128 8B          (__DATA,__data)                   type: S_REGULAR  offset: 33056
LC_SEGMENT_64        cmdsize: 72     segname: __LINKEDIT     file: 0x0000c000-0x0000c728 1.79KB     vm: 0x10000c000-0x100010000 16.00KB   prot: 1/1
```

## [LC_DYLD_INFO_ONLY](docs/LC_DYLD_INFO.md)
```
$ ./macho_parser --dyld-info sample.out
LC_DYLD_INFO_ONLY    cmdsize: 48     export_size: 192
  rebase_off   : 49152        rebase_size   : 24
  bind_off     : 49176        bind_size     : 184
  weak_bind_off: 0            weak_bind_size: 0
  lazy_bind_off: 49360        lazy_bind_size: 80
  export_off   : 49440        export_size   : 192
```
### Rebase
```
$ ./macho_parser --rebase sample.out
  Rebase Table:
__DATA_CONST,__mod_init_func      0x100004010  pointer  value(0x100003E70)
__DATA_CONST,__cfstring           0x100004028  pointer  value(0x100003F89)
__DATA_CONST,__objc_classlist     0x100004038  pointer  value(0x1000080F8)
__DATA_CONST,__objc_nlclslist     0x100004040  pointer  value(0x1000080F8)
__DATA,__la_symbol_ptr            0x100008000  pointer  value(0x100003F70)
__DATA,__la_symbol_ptr            0x100008008  pointer  value(0x100003F40)
__DATA,__la_symbol_ptr            0x100008010  pointer  value(0x100003F5C)
__DATA,__la_symbol_ptr            0x100008018  pointer  value(0x100003F66)
...
```
```
$ ./macho_parser --rebase --opcode sample.out
  Rebase Opcodes:
0x0000 REBASE_OPCODE_SET_TYPE_IMM (REBASE_TYPE_POINTER)
0x0001 REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB (2, 0x00000010) -- __DATA_CONST
0x0003 REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB (0x00000010)
0x0005 REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB (0x00000008)
0x0007 REBASE_OPCODE_DO_REBASE_IMM_TIMES (2)
0x0008 REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB (3, 0x00000000) -- __DATA
0x000A REBASE_OPCODE_DO_REBASE_IMM_TIMES (4)
0x000B REBASE_OPCODE_ADD_ADDR_IMM_SCALED (1)
...
```

### Bind / Lazy Bind / Weak Bind
```
$ ./macho_parser --bind [--lazy-bind] [--weak-bind] sample.out
  Binding Table:
__DATA_CONST,__got        0x100004000  pointer  flat-namespace        addend(0)  _c_extern_weak_function (weak import)
__DATA_CONST,__got        0x100004008  pointer  libSystem.B.dylib     addend(0)  dyld_stub_binder
__DATA_CONST,__cfstring   0x100004018  pointer  CoreFoundation        addend(0)  ___CFConstantStringClassReference
__DATA,__objc_data        0x100008100  pointer  libobjc.A.dylib       addend(0)  _OBJC_CLASS_$_NSObject
__DATA,__objc_data        0x1000080D0  pointer  libobjc.A.dylib       addend(0)  _OBJC_METACLASS_$_NSObject
__DATA,__objc_data        0x1000080D8  pointer  libobjc.A.dylib       addend(0)  _OBJC_METACLASS_$_NSObject
__DATA,__objc_data        0x1000080E0  pointer  libobjc.A.dylib       addend(0)  __objc_empty_cache
__DATA,__objc_data        0x100008108  pointer  libobjc.A.dylib       addend(0)  __objc_empty_cache
```
```
$ ./macho_parser --bind --opcode sample.out
  Binding Opcodes:
0x0000 BIND_OPCODE_SET_DYLIB_SPECIAL_IMM (BIND_SPECIAL_DYLIB_FLAT_LOOKUP)
0x0001 BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM (BIND_SYMBOL_FLAGS_WEAK_IMPORT, _c_extern_weak_function)
0x001A BIND_OPCODE_SET_TYPE_IMM (BIND_TYPE_POINTER)
0x001B BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB (2, 0x00000000) -- __DATA_CONST
0x001D BIND_OPCODE_DO_BIND ()
0x001E BIND_OPCODE_SET_DYLIB_ORDINAL_IMM (2) -- libSystem.B.dylib
0x001F BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM (0, dyld_stub_binder)
0x0031 BIND_OPCODE_DO_BIND ()
...
```
### [Export](../exported_symbol/README.md)
```
$ ./macho_parser --export sample.out
  Exported Symbols (Trie):
  _
    _mh_execute_header (data: 0000)
    c_
      constructor_function (data: 00f07c)
      used_function (data: 00807d)
      weak_import_function (data: 00907d)
    main (data: 00a07d)
    OBJC_
      METACLASS_$_SimpleClass (data: 00d08102)
      CLASS_$_SimpleClass (data: 00f88102)
```

## [LC_SYMTAB](docs/LC_SYMTAB.md)
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

## [LC_DYSYMTAB](docs/LC_DYSYMTAB.md)
```
$ ./macho_parser --dysymtab --local --extdef --undef --indirect sample.out
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

## [LC_FUNCTION_STARTS](docs/LC_FUNCTION_STARTS.md)
```
$ ./macho_parser -c LC_FUNCTION_STARTS sample.out
LC_FUNCTION_STARTS   cmdsize: 16     dataoff: 0xc1e0 (49632)   datasize: 8
  0x100003e70  _c_constructor_function
  0x100003e80  _c_used_function
  0x100003e90  _c_weak_import_function
  0x100003ea0  _main
  0x100003f00  +[SimpleClass load]
```

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

## [LC_*_DYLIB](docs/LC_dylib.md)
```
$ ./macho_parser --dylibs sample.out
LC_LOAD_DYLIB        cmdsize: 48     @rpath/my_dylib.dylib
LC_LOAD_DYLIB        cmdsize: 56     /usr/lib/libSystem.B.dylib
LC_LOAD_DYLIB        cmdsize: 104    /System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation
LC_LOAD_DYLIB        cmdsize: 96     /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation
LC_LOAD_DYLIB        cmdsize: 56     /usr/lib/libobjc.A.dylib
```

## [LC_RPATH](docs/LC_dylib.md)
```
$ ./macho_parser --command LC_RPATH sample.out
LC_RPATH             cmdsize: 24     build
```

## [LC_BUILD_VERSION](docs/LC_BUILD_VERSION.md)
```
$ ./macho_parser --build-version sample.out
LC_BUILD_VERSION     cmdsize: 32     platform: MACOS   minos: 12.0.0   sdk: 12.0.0
    tool:  LD   version: 711.0.0
```

## [LC_CODE_SIGNATURE](docs/LC_CODE_SIGNATURE.md)
```
$ ./macho_parser --code-signature --code-directory --entitlement --blob-wrapper -v SampleApp.app/SampleApp
LC_CODE_SIGNATURE    cmdsize: 16     dataoff: 0x214e0 (136416)   datasize: 20384
SuperBlob: magic: CSMAGIC_EMBEDDED_SIGNATURE, length: 7162, count: 5
  Blob 0: type: 0000000, offset: 52, magic: CSMAGIC_CODEDIRECTORY, length: 1430
    version      : 0x20400
    flags        : 0
    hashOffset   : 342
    identOffset  : 88
    nSpecialSlots: 7
    nCodeSlots   : 34
    codeLimit    : 136416
    hashSize     : 32
    hashType     : SHA256
    platform     : 0
    pageSize     : 4096
    identity     : me.qyang.SampleApp
    CDHash       : 353b6287288167ddfab1b0dff6d628d774045e941be05d6960662da3d1878ee3

    Slot[ -7] : e7f1d4635ba1128f12935ec8659106d194ef48e907e3750a5263bdfc62b268f0
    Slot[ -6] : 0000000000000000000000000000000000000000000000000000000000000000
    Slot[ -5] : 5c605b825812dc227e5162acba5f8af22b61b330c703397b4586f266e4534fe9
    Slot[ -4] : 0000000000000000000000000000000000000000000000000000000000000000
    ...

  Blob 1: type: 0x00002, offset: 1482, magic: CSMAGIC_REQUIREMENTS, length: 188
    Requirement[0]: offset: 20, length: 168
      identifier "me.qyang.SampleApp" and anchor apple generic and certificate leaf[subject.CN] = "Apple Development: yangq.nj@gmail.com (J5K827UFE4)" and certificate 1[field.1.2.840.113635.100.6.2.1] /* exists */

  Blob 2: type: 0x00005, offset: 1670, magic: CSMAGIC_EMBEDDED_ENTITLEMENTS, length: 495
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
        <key>application-identifier</key>
        <string>YK72VSF9J6.me.qyang.SampleApp</string>
        ...
</dict>
</plist>

  Blob 3: type: 0x00007, offset: 2165, magic: UNKNOWN(0xfade7172), length: 205
  Blob 4: type: 0x10000, offset: 2370, magic: CSMAGIC_BLOBWRAPPER, length: 4792
    PKCS7:
      type: pkcs7-signedData (1.2.840.113549.1.7.2)
      d.sign:
        version: 1
        md_algs:
            algorithm: sha256 (2.16.840.1.101.3.4.2.1)
            parameter: NULL
   ...
```

