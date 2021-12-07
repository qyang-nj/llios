# Binding Information
In [this article](../exported_symbol/README.md), we talked about exported symbols, which are symbols provided by a binary. Before using, the symbols need to be bound. Binding is the process to connect a symbol between the consumer (e.g. an executable) and provider (usually a dylib). Binding information is the data stored in the consumer to facilitate the binding process.

## LC_DYLD_INFO(_ONLY)
Like exported symbols, binding information are also stored in [`LC_DYLD_INFO(_ONLY)`](../macho_parser/docs/LC_DYLD_INFO.md). There are three kinds of binding information, non-lazy bind, lazy bind and weak bind. Their locations are specified by `bind_off`, `lazy_bind_off` and `weak_bind_off` respectively. We will focus on non-lazy bind in this article and the other two are very similar.
```
$ ./macho_parser sample.out --dyld-info
LC_DYLD_INFO_ONLY    cmdsize: 48     export_size: 192
  ...
  bind_off     : 49176        bind_size     : 184
  weak_bind_off: 0            weak_bind_size: 0
  lazy_bind_off: 49360        lazy_bind_size: 80
  ...
```

## Opcodes
The binding information is a table, where each row denotes a symbol to be bound. The table is encoded in a very efficient way, known as opcodes. Instead of recoding all the information for a row, only the differences from previous row are stored.

How opcodes can encode a table? Let's take a simplified example. Say we have a sequence of opcode like below.

```
DO A = 1
DO B = 0x1000
DO C = "foo"
NEXT
DO B += 0x8
DO C = "boo"
NEXT
DO A = 2
DO B += 0x10
NEXT
END
```

At the beginning, A, B and C are set to the default value 0, 0 and null. Each opcode makes some changes on the data. `NEXT` means finish current row and go to the next row. Following along the opcodes, we can construct a table at the end.

|A|B|C|
|-|-|-|
|1|0x1000|foo|
|1|0x1008|boo|
|2|0x1018|boo|

The advantage of opcode is pretty clear. In row 2, instead of storing the whole number 0x1008 which takes two bytes, we can just store the number 8, that can be embedded into the same byte with the opcode (described below). Also, we don't need to store "boo" twice for row 2 and row 3, as we only record the differences.

Now think A, B, C are segment index, address and symbol name. This is basically how opcode works in binding information.

The real opcode is very compact but powerful. Only the 4 high-order bits are the opcode, so there can be up to 16 different opcodes. Each opcode can have zero or more operands. If the value of an operand is less than 16, it can be stored in the 4 low-order bits, called immediate. If the value is more than 16, it is stored as LEB128 number in the following bytes.

The opcodes are defined in [<mach-o/loader.h>](https://github.com/qyang-nj/llios/blob/e9f09d171b3845a04b081957d43d8f3b1a4917b6/apple_open_source/xnu/EXTERNAL_HEADERS/mach-o/loader.h#L1423-L1455).


The full parsing logic can be found at [`dyld_info.cpp`](`https://github.com/qyang-nj/llios/blob/main/macho_parser/sources/dyld_info.cpp`)

The actual opcode looks like below. (`macho_parser --bind --opcode sample.out`)
```
0x001D BIND_OPCODE_DO_BIND ()
0x001E BIND_OPCODE_SET_DYLIB_ORDINAL_IMM (2) -- libSystem.B.dylib
0x001F BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM (0, dyld_stub_binder)
0x0031 BIND_OPCODE_DO_BIND ()
0x0032 BIND_OPCODE_SET_DYLIB_ORDINAL_IMM (3) -- CoreFoundation
0x0033 BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM (0, ___CFConstantStringClassReference)
0x0056 BIND_OPCODE_ADD_ADDR_ULEB (0x00000008)
0x0058 BIND_OPCODE_DO_BIND ()
0x0059 BIND_OPCODE_SET_DYLIB_ORDINAL_IMM (5) -- libobjc.A.dylib
0x005A BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM (0, _OBJC_CLASS_$_NSObject)
0x0072 BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB (3, 0x00000100) -- __DATA
0x0075 BIND_OPCODE_DO_BIND ()
0x0076 BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM (0, _OBJC_METACLASS_$_NSObject)
0x0092 BIND_OPCODE_ADD_ADDR_ULEB (0xffffffffffffffc8)
0x009D BIND_OPCODE_DO_BIND ()
0x009E BIND_OPCODE_DO_BIND ()
0x009F BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM (0, __objc_empty_cache)
0x00B3 BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED (4)
0x00B4 BIND_OPCODE_DO_BIND ()
0x00B5 BIND_OPCODE_DONE
```

The actual table is like this (`macho_parser --bind --opcode sample.out`)
```
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

A few things to note
* Binding info doesn't store the segment or section name. Instead it stores the segment index and the offset to he beginning of the segment. The segment index indicates the `LC_SEGMENT(_64)`. We can use the segment and the offset to calculate the address. Thus we can know which section it is at.
* Binding info doesn't store the dylib name.
* Binding info does contain the symbol names as the literal strings.
* I don't how `addend` is used. It seems they are always 0.


## Lazy Binding
The lazy binding process is explained [here](../dynamic_linking/README.md#lazy-binding). A quick recap: the lazy binding pointer points to a helper method initially. The helper method binds the symbol at the first time of use.

As we can tell, the process is different than non-lazy binding and isn't required to be bound at launch time (that's why it's called lazy). Because of this, it doesn't seem to be necessary to store a lazy binding tables. However, that's not the case. Lazy binding information is also stored in the `LC_DYLD_INFO(_ONLY)` as a table. Why? It's because `dyld` allows us to force bind all lazy symbols at launch time, simply by setting `DYLD_BIND_AT_LAUNCH` environment variable.


## Debugging Tools
### dyldinfo
Besides using my [macho parser](../macho_parser), all the binding information, including the opcodes can be examined by `dyldinfo` tool, which should be more trustworthy.
```
xcrun dyldinfo -bind [-weak_bind] [-lazy_bind] [-opcodes] /bin/ls
```

### dyld
We can also set `DYLD_PRINT_BINDINGS` environment variable before launching the program. This instructs `dyld` to print whenever a symbol is bound.
```
DYLD_PRINT_BINDINGS=1 ls
```
