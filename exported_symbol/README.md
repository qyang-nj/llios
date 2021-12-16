# Exported Symbol
Exported symbols are the public APIs provided by a dynamic library. For a long time, I thought the exported symbols were stored in the symbol table (It's not wrong though). Surprisingly exported symbols are stored somewhere in a more efficient way, smaller on disk and faster at runtime. In this article, we will dive into the details of the export information (short for export info).

It may not be obvious that an executable can also have exported symbols (use cases are explained in below "strip" section). We will anatomize the following sample code which contains a few public symbols. We are using C here as it does the least name mangling than other languages.

``` c
// clang -o sample.out sample.c
void llios_func() {}
void llios_func_2nd() {}
int llios_int = 0;

int main() { return 0; }
```
*(The sample code and build script are in this directory.)*

## Dyld Info
### LC_DYLD_INFO(_ONLY)
As mentioned above, export info is not stored in symbol table (`LC_SYMTAB`). Instead, they are in the [`LC_DYLD_INFO(_ONLY)`](../macho_parser/docs/LC_DYLD_INFO.md) load command. With the help of `otool -l`, we're able to see the offset and the size of export info.

```
$ otool -l sample.out
Load command 4
            cmd LC_DYLD_INFO_ONLY
            ...
     export_off 16384
    export_size 88
```
Having the offset and size, we can simply hex-dump the export info content. Later we will demystify this byte by byte.
```
$ xxd -s 16384 -l 88 a.out
00004000: 0001 5f00 0500 035f 6d68 5f65 7865 6375  .._...._mh_execu
00004010: 7465 5f68 6561 6465 7200 296c 6c69 6f73  te_header.)llios
00004020: 5f00 2d6d 6169 6e00 4a02 0000 0000 0266  _.-main.J......f
00004030: 756e 6300 3a69 6e74 004f 0300 807f 015f  unc.:int.O....._
00004040: 326e 6400 4503 0090 7f00 0300 a07f 0004  2nd.E...........
00004050: 0080 8001 0000 0000                      ........
```

Before parsing the bytes by ourselves, just let you know that we can use `dyldinfo -export` to dump all the exported symbols and use them to verify our parsing results.
```
$ xcrun dyldinfo -export sample.out
export information (from trie):
0x100000000  __mh_execute_header
0x100003F80  _llios_func
0x100003F90  _llios_func_2nd
0x100003FA0  _main
0x100004000  _llios_int
```

### LC_DYLD_EXPORTS_TRIE
If the binary is targeted at iOS 14+ or is linked with `-fixup_chains` linker flag, the same information is stored in `LC_DYLD_EXPORTS_TRIE` load command instead. The detail of this change is discussed at [Chained Fixups](../dynamic_linking/chained_fixups.md).

## Export Trie
The export info is actually a trie. A [trie](https://en.wikipedia.org/wiki/Trie) is a tree structure that is used for accelerating searching strings. It has nodes and edges. Different from the trie we were taught in text book, in an export trie, an edge is a string, and a node stores associated data.

The export trie in the Mach-O file is a bit stream that is encoded by [ULEB128](https://en.wikipedia.org/wiki/LEB128). Although I'm not going into the algorithm of ULEB128, the thing worth mentioning is that if a number is <= 128 (0x7f), ULEB128 representation is the same as `uint8`. Since in this sample the size of export is only 88 bytes, for the sake of simplicity, we will pretend they are byte values instead of decoding ULEB128.

It's really hard to describe how the trie is encoded in a few sentences. However, if I break each node, use relative address, and convert ASCII values to the characters, visually the above mystery hex dump can be translated into the form below.

```
4000: 00 01 "_" 05
 +05: 00 03 "_mh_execute_header" 29
            "_llio_" 2d
            "main" 4a
 +29: 02 (00 00) 00
 +2d: 00 02 "func" 3a
            "int" 4f
 +3a: 03 (00 80 7f) 01 "_2nd" 45
 +45: 03 (00 90 7f) 00
 +4a: 03 (00 a0 7f) 00
 +4f: 04 (00 80 80 01) 00
```

Hopefully this is much easier to read and with a little explantion, you can understand how the trie is embeded.
```
[offset]: [size] ([data]) [children count] [edge 1 string] [child 1 offset]
                                           [edge 2 string] [child 2 offset]

offset        : the offset relative to the beginning of the export info
size          : the size of the data. If the size is larger than 0, the node is a terminal node.
data          : information about the symbol. Only terminal node has data.
children count: the number of this node's children
edge n string : the zero-terminated string of the edge from this node to its nth child
child n offset: the nth child's location. It's the offset relative to the beginning of the export info.
```

One important concept for a trie is **terminal node**. In the export trie, concatenating all the edges (string) from the root to a terminal node is a complete symbol. Terminal node also stores the data associated to that symbol, like flags, address, etc. Please note terminal node can also have children, which is demonstrated by the symbol `_llios_func` and `_llios_func_2nd`.

This is what the trie actually looks like. The green nodes are the terminal nodes. Traversing the trie, we can get five exported symbols which are the same as we saw in the above `dyldinfo -export` command.

![Trie Graph](../articles/images/Export%20Trie.png)

To learn more, [here](../macho_parser/dyld_info.c) is the simple trie parser in the [macho parser](../macho_parser), and [here](https://github.com/opensource-apple/dyld/blob/3f928f32597888c5eac6003b9199d972d49857b5/launch-cache/MachOTrie.hpp) is the full-fledged parser in `dyld`.

## Control export symbols
By default, global symbols are exported. Many linker flags can control what symbols to export, `-exported_symbols_list`, `-exported_symbol`, `-unexported_symbols_list`, `-unexported_symbol`, `-reexported_symbols_list`, `-alias`, etc. More details are in the `man ld`.

## Strip
Using `strip` without any arguments will strip out global symbols, in both symbol table and export info. When we release an app, we want to strip out all the global symbols. It's very important to know that **`strip` only prunes the export trie but doesn't resize the export info**, so the export info section will end up with lots of useless 0s. We actually found there were 14MB 0s in our released app. We got rip of all the export info by providing `-exported_symbols_list /dev/null` to the linker.

```
$ strip sample.out
$ xxd -s 16384 -l 88 a.out
00004000: 0001 5f5f 6d68 5f65 7865 6375 7465 5f68  ..__mh_execute_h
00004010: 6561 6465 7200 1702 0000 0000 0000 0000  eader...........
00004020: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00004030: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00004040: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00004050: 0000 0000 0000 0000                      ........
```

For more details, see the [source code](https://github.com/opensource-apple/cctools/blob/fdb4825f303fd5c0751be524babd32958181b3ed/misc/strip.c#L3944) of `strip` and the [prune_trie](https://github.com/apple-opensource/ld64/blob/e28c028b20af187a16a7161d89e91868a450cadc/src/other/PruneTrie.cpp#L45) method of `ld64`.

There is one use case that an executable needs to preserve the exported symbol — loading plugins. The plugins (`MH_BUNDLE`) is loaded by an executable (`MH_EXECUTE`) and uses the executable's exported symbols. One example is that a test runner loads test cases as plugins (see [xctest](../xctest)). I can't think of a case why an iOS app needs to load plugins at runtime.

## Symbol Table
So far I'm still puzzled by one question. As I said at the beginning, I was not wrong that pubic symbols are stored in the symbol table, which is a different parts of macho file. We can dump the content of symbol table and string table to verify this.
```
$ nm -pg sample.out
0000000100000000 T __mh_execute_header
0000000100003f80 T _llios_func
0000000100003f90 T _llios_func_2nd
0000000100004000 S _llios_int
0000000100003fa0 T _main
                 U dyld_stub_binder

# Using macho_parser to find the offset and size of string table
$ xxd -s 16576 -l 88 sample.out
000040c0: 2000 5f5f 6d68 5f65 7865 6375 7465 5f68   .__mh_execute_h
000040d0: 6561 6465 7200 5f6c 6c69 6f73 5f66 756e  eader._llios_fun
000040e0: 6300 5f6c 6c69 6f73 5f66 756e 635f 326e  c._llios_func_2n
000040f0: 6400 5f6c 6c69 6f73 5f69 6e74 005f 6d61  d._llios_int._ma
00004100: 696e 0064 796c 645f 7374 7562 5f62 696e  in.dyld_stub_bin
00004110: 6465 7200 0000 0000                      der.....
```

Obviously macho file stores public symbols twice - a trie in the export info and a list in symbol table.

**Can we safely remove public symbols from symbol tables but keep the export info?** If so, how? Nether `strip` nor `ld` has a flag to just do that. If not, how are those symbols in symbol table used by `dyld`?
