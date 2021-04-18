# MachO Parser
To learn the MachO format, the best way is to build a parser from scratch. It helps me understand how MachO format is exactly laid out.

This parser actually turns out to be a super light version of the combination of  `otool`, `nm`, `strings`, etc.

## Notes
### __mod_init_func
`(__DATA,__mod_init_func)` or `(__DATA_CONST,__mod_init_func)`

This is the section that contains of a list of function pointers, which will be executed by `dyld` before `main`. Those are functions with `__attribute__((constructor))` and they will affect the app launch time.

Once we have the function pointer address, we can use `atos` to query the function name.
```
> xcrun atos -o sample/sample 0x100003f20
c_constructor_function (in sample) + 0
```
