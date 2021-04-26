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
Another interesting line is line 7.
```
0000000100003f89	callq	0x100003f96
```
From `main.c`, we know the only method is called is `lib_func`, so this line must be calling into it.

`0x3f96` is in the `__stubs` section. There is only one `jmpq` instruction.
```
$ otool -v -s __TEXT __stubs a.out
Contents of (__TEXT,__stubs) section
0000000100003f96	jmpq	*0x4064(%rip)
```

The star sign in `*0x4064(%rip)` indicates this is indirect addressing. It will jump to the address stored in `0x4064(%rip)`. As always, the value of `%rip` is the address of the next instruction. However this is only instruction in this section. Call the same otool command without `-v`.

```
$ otool -s __TEXT __stubs a.out
Contents of (__TEXT,__stubs) section
0000000100003f96 ff 25 64 40 00 00
```
We know the size `jmpq` is 6 bytes, so the `%rip` is `0x3f9c` (0x3f96 + 0x6). Hence `0x4064(%rip)` is `0x8000` (0x3F9C + 0x4064). Where is `0x8000` then? It's in the `__la_symbol_ptr` section.
```
$ otool -s __DATA __la_symbol_ptr a.out
Contents of (__DATA,__la_symbol_ptr) section
0000000100008000	ac 3f 00 00 01 00 00 00
```

Same as `__got` section, `__la_symbol_ptr` is also an array 64-bit pointers which are associated to indirect symbols. Using the same approach described before, `0x8000` is for the symbol `_lib_func`, which is what we expect.
```
$ otool -I a.out
Indirect symbols for (__DATA,__la_symbol_ptr) 1 entries
address            index
0x0000000100008000     3
```

Different from `__got`, the current value of `0x8000` is not `0x0`. Instead it's `0x3fac` (endianness). Interestingly, `0x3fac` is in the section `__stub_helper`.
```
$ otool -v -s __TEXT __stub_helper a.out
Contents of (__TEXT,__stub_helper) section
0000000100003f9c	leaq	0x4065(%rip), %r11
0000000100003fa3	pushq	%r11
0000000100003fa5	jmpq	*0x5d(%rip)
0000000100003fab	nop
0000000100003fac	pushq	$0x0
0000000100003fb1	jmp	0x100003f9c
```
Please note that `0x3fac` is at line 5. Following the code, it jumps to `0x3f9c` which is line 1. Eventually it jumps to `*0x5d(%rip)` (indirect addressing again). We should be good at this now. `0x5d(%rip)` is `0x4008` (0x3fab + 0x5d). It seems we have seen this location before. Yes, it's the 2nd element in `__got`, which is `dyld_stub_binder`. As you may still remember, `__got` is the non-lazy binding section and the address of `dyld_stub_binder` will be written there at launch time.

If you haven't got lost so far, `callq	0x100003f96` instruction actually calls into [`dyld_stub_binder`](https://opensource.apple.com/source/dyld/dyld-195.5/src/dyld_stub_binder.s.auto.html). This is a special method provided by `dyld`. It finds the address of the symbol (it's `_lib_func` in our case) and writes the address in the `__la_symbol_ptr`.

Here is what happens when calling a method in a dylib. The program calls into code in (__TEXT,__stub) which reads the address stored in `__la_symbol_ptr` and jumps to that. At the first time, that address is pointing to (__TEXT,__stub_helper) which in turn calls into `dyld_stub_binder`. `dyld_stub_binder` finds the symbol in the dylib, writes it back to `__la_symbol_ptr` and jumps to the real method. Next time calling the same method, `__la_symbol_ptr` has the real address, so the program can jump to it directly.
