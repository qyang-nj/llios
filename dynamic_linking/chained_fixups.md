# Chained Fixups

LC_DYLD_CHAINED_FIXUPS
LC_DYLD_INFO

Chained Fixups is a new way to replace old dyld info ()

no longer has lazy bind.

-fixup_chains

https://github.com/qyang-nj/llios/blob/1f111edc87adbca68c336d3ab501e3ca4a1f2356/apple_open_source/ld64/src/ld/Options.cpp#L5233-L5238

LC_DYLD_CHAINED_FIXUPS is [enabled by default if the binary is built for device and targeted at iOS 14+](https://github.com/qyang-nj/llios/blob/1f111edc87adbca68c336d3ab501e3ca4a1f2356/apple_open_source/ld64/src/ld/Options.cpp#L5233-L5238). We can also manually enable it by passing `-fixup_chains` to `ld`.

## Dumping info

Unlike `LC_DYLD_INFO`, `dyldinfo` doesn't not support chained fixups at this moment. You can use `otool` with option `-fixup_chains`, but it only shows imports, not rebase. My [macho parser](../macho_parser) is able to print out most of the information, including rebase and bind info of each segment, as well as the imports table.
```
$ macho_parser/parser -c LC_DYLD_CHAINED_FIXUPS -v ../macho_parser/sample.out

LC_DYLD_CHAINED_FIXUPS cmdsize: 16     dataoff: 0xc000 (49152)   datasize: 296
    CHAINED FIXUPS HEADER
    fixups_version : 0
    starts_offset  : 0x20 (32)
    imports_offset : 0x68 (104)
    symbols_offset : 0x84 (132)
    imports_count  : 7
    imports_format : 1 (DYLD_CHAINED_IMPORT)
    symbols_format : 0 (UNCOMPRESSED)

    IMPORTS
    [0] lib_ordinal: 1   weak_import: 0   name_offset: 1 (_objc_opt_self)
    [1] lib_ordinal: 3   weak_import: 0   name_offset: 16 (_swift_allocObject)
    [2] lib_ordinal: 3   weak_import: 0   name_offset: 35 (_swift_deallocClassInstance)
    [3] lib_ordinal: 1   weak_import: 0   name_offset: 63 (__objc_empty_cache)
    ...

    SEGMENT 3 (offset: 48)
    size: 24
    page_size: 0x4000
    pointer_format: 2 (DYLD_CHAINED_PTR_64)
    segment_offset: 0x8000
    max_valid_pointer: 0
    page_count: 1
    page_start: 24
        SEGMENT 3, PAGE 0 (offset: 24)
        0x00008018 REBASE > target: 0x100003f10   high8: 0
        0x00008060 REBASE > target: 0x100003f10   high8: 0
        0x00008090 BIND   > ordinal: 4   addend: 0    reserved: 0
        0x00008098 BIND   > ordinal: 4   addend: 0    reserved: 0
        ...
```
