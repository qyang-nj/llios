# MachO Parser
To learn the MachO format, the best way is to build a parser from scratch. It helps me understand how MachO format is exactly laid out.

This parser actually turns out to be a super light version of the combination of  `otool`, `nm`, `strings`, etc.

## Notes
### __mod_init_func
`(__DATA,__mod_init_func)` or `(__DATA_CONST,__mod_init_func)`
Type: S_MOD_INIT_FUNC_POINTERS

This is the section that contains of a list of function pointers, which will be executed by `dyld` before `main`. Those are functions with `__attribute__((constructor))` or Objective-C's `+load` method. Functions here will affect the app launch time.
