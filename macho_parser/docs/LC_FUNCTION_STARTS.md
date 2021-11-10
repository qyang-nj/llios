# LC_FUNCTION_STARTS
`LC_FUNCTION_STARTS` load command stores a list of all function addresses. The function addresses are debugging information and used by debugger. To strip this section, pass `-no_function_starts` to `ld`. This is usually done in the release builds to save app size.

 This load command lives in `__LINKEDIT` segment, using [the generic `linkedit_data_command`](https://github.com/qyang-nj/llios/blob/7652e7286bcd1cf98bcc5c92d77bb071cb097119/apple_open_source/xnu/EXTERNAL_HEADERS/mach-o/loader.h#L1189-L1202). The function addresses are encoded by a list of [ULEB128](https://en.wikipedia.org/wiki/LEB128) numbers. The first number is the first function's offset to `__TEXT`'s `vmaddr`. The following numbers are the offset to the previous address.

In the example of our sample program, we hex dump the content of `LC_FUNCTION_STARTS`.
```
$ otool -l sample.out | grep LC_FUNCTION_STARTS -A3
      cmd LC_FUNCTION_STARTS
  cmdsize 16
  dataoff 49632
 datasize 8

$ xxd -s 49632 -l 8 sample.out
0000c1e0: f07c 1010 1060 0000
```
These are a sequence of ULEB128-encoded numbers: 0x3E70, 0x10, 0x10, 0x10, 0x60. Since `vmaddr` of `__TEXT` is 0x100000000, the function addresses are
```
0x100003E70   (0x100000000 + 0x3e70)
0x100003E80   (0x100003E70 + 0x10)
0x100003E90   (0x100003E80 + 0x10)
0x100003EA0   (0x100003EA0 + 0x10)
0x100003F00   (0x100003E80 + 0x60)
```

The function starts are only addresses, no function names. We can look up symbol table to get the function names.

```
$ xcrun dyldinfo -function_starts sample.out
0x100003E70   _c_constructor_function
0x100003E80   _c_used_function
0x100003E90   _c_weak_import_function
0x100003EA0   _main
0x100003F00   +[SimpleClass load]
```



## More
[Symbolication: Beyond the basics](https://developer.apple.com/videos/play/wwdc2021/10211/), starting at 14:22.
