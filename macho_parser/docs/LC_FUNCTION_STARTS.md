# LC_FUNCTION_STARTS
`LC_FUNCTION_STARTS` load command stores a list of all function addresses. It lives in `__LINKEDIT` segment, using the generic `linkedit_data_command`. The function addresses are encoded by a list of [ULEB128](https://en.wikipedia.org/wiki/LEB128) numbers. The first number is the first function's offset to `__TEXT`'s `vmaddr`. The following numbers are the offset to the previous address. In the example of our sample program, here is its `LC_FUNCTION_STARTS`.

``` c
struct linkedit_data_command {
    uint32_t	cmd;
    uint32_t	cmdsize;    /* sizeof(struct linkedit_data_command) */
    uint32_t	dataoff;    /* file offset of data in __LINKEDIT segment */
    uint32_t	datasize;   /* file size of data in __LINKEDIT segment  */
};
```

## ULEB128


## Function Starts

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

The function starts are only addresses, no function names. We can use `dyldinfo -function_starts` to dump the function addresses, which will also look up symbol table to get function names.
```
$ xcrun dyldinfo -function_starts sample.out
0x100003E70   _c_constructor_function
0x100003E80   _c_used_function
0x100003E90   _c_weak_import_function
0x100003EA0   _main
0x100003F00   +[SimpleClass load]
```

To strip this section, pass `-no_function_starts` to `ld`. This is usually done in release builds to save app size.


## More
[Symbolication: Beyond the basics](https://developer.apple.com/videos/play/wwdc2021/10211/), starting at 14:22.
