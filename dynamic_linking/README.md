# How does dynamic linking works?

This article is trying to explain how dynamic linking works at the Mach-O level.

## Background knowledge
### Indirect symbol table
(TBD)
### RIP relative addressing
(TBD)

## Sample Code
First, let's build a simple dylib example. `lib.c` will be will be built into a dylib, which `main.c` will link against.

```c
// lib.c
#include <stdio.h>

char *lib_str = "str";

void lib_func(char *str) {
    printf("%s\n", str);
}

```

``` c
// main.c
extern char *lib_str;
extern void lib_func(char *);

int main() {
    lib_func(lib_str);
    return 0;
}
```
(The sample code and build script are in this directory. Feel free to build the code by yourself and follow along the article.)

Then we disassemble the `__text` section (where the code exist) by `otool`.

``` bash
$ otool -tv a.out
a.out:
(__TEXT,__text) section
_main:
0000000100003f70	pushq	%rbp
0000000100003f71	movq	%rsp, %rbp
0000000100003f74	subq	$0x10, %rsp
0000000100003f78	movq	0x81(%rip), %rax
0000000100003f7f	movl	$0x0, -0x4(%rbp)
0000000100003f86	movq	(%rax), %rdi
0000000100003f89	callq	0x100003f96
0000000100003f8e	xorl	%eax, %eax
0000000100003f90	addq	$0x10, %rsp
0000000100003f94	popq	%rbp
0000000100003f95	retq
```

## Non-lazy binding
The first interesting part is line 4.
```
0000000100003f78    movq    0x81(%rip), %rax
0000000100003f7f    ...
```
This line  accesses the value at `0x81(%rip)`. `%rip` is always pointing to the next instruction, so the current value  `0x3f7f`. Thus `0x81(%rip)` is `0x4000` (0x3f7f + 0x81), which is in the `__got` section.

```
$ otool -s __DATA_CONST __got a.out
a.out:
Contents of (__DATA_CONST,__got) section
0000000100004000	00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

The `__got` section is `S_NON_LAZY_SYMBOL_POINTERS` (`struct section_64.flags & SECTION_TYPE`). The section is an array of 64-bit pointers. Currently there are two elements. Each element is associated to an indirect symbol.

```
$ otool -I a.out
...
Indirect symbols for (__DATA_CONST,__got) 2 entries
address            index
0x0000000100004000     4
0x0000000100004008     5
...
```

With the knowledge of indirect symbol table, we know that `0x4000` is associated to the 4th element in the symbol table, which is `_lib_str`, the global variable in the dylib.
```
$ nm -ap a.out | nl -v 0
     0	0000000100008008 d __dyld_private
     1	0000000100000000 T __mh_execute_header
     2	0000000100003f70 T _main
     3	                 U _lib_func
     4	                 U _lib_str
     5	                 U dyld_stub_binder
```
(`-a` and `-p` options are really important here. It prints all symbols and preserves the order in the symbol table.)


The values in `__got` are all 0x0 in the file, because we won't know the values until runtime. `dyld`, at launch time, will bind non-lazy symbols and write the value into the section. That is why `__got` in the the writable __DATA section. (At this moment I'm not sure the difference between `__DATA` and `__DATA_CONST`). Global variables are non-lazily binded while functions are usually lazily binded.

Another symbol in `__got` is `dyld_stub_binder`. We will talk about it in the next section.

## Lazy binding


## References

