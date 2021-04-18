# MachO Parser
To learn the MachO format, the best way is to build a parser from scratch. It helps understand how MachO format is exactly laid out.

This parser actually turns out to be a super light version of the combination of  `otool`, `nm`, `strings`, etc.

## Usage
Build and run
``` bash
./build.sh
./parser /path/to/a/macho
```

There is a sample that you can change the code and observe how the macho is changed.
```
cd sample && ./build.sh && cd ..
./parser sample/sample
```

## Notes
### __mod_init_func
`(__DATA,__mod_init_func)` or `(__DATA_CONST,__mod_init_func)`

This is the section that contains of a list of function pointers, which will [be executed by `dyld`](https://github.com/opensource-apple/dyld/blob/3f928f32597888c5eac6003b9199d972d49857b5/src/ImageLoaderMachO.cpp#L1815~L1847) before `main`. Those are functions with `__attribute__((constructor))` and they will affect the app launch time.

Once we have the function pointer address, we can use `atos` to query the function name.
```
> xcrun atos -o sample/sample 0x100003f20
c_constructor_function (in sample) + 0
```

> Please note that ObjC's `+load` methods will also be executed before `main`, but uses a different mechanism. See below "+load in ObjC" section.

### `+load` in ObjC
The MachO that has Objective-C code will have two sections, `(__DATA_CONST,__objc_classlist)` and `(__DATA_CONST,__objc_nlclslist)`. `__objc_classlist` includes the addresses of all ObjC classes, while `__objc_nlclslist` contains only *non-lazy* classes. [Non-lazy classes are classes that have `+load` method](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-runtime-new.mm#L2806~L2812) and will be loaded at launch time.

#### How `+load` is executed during startup?
1. dyld calls [_objc_init](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-os.mm#L803~L831), where [a notification is registered when an image is loaded](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-os.mm#L830).
2. In the notification callback, [load_images](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-runtime-new.mm#L2157~L2193), it [calls the load methods](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-loadmethod.mm#L306~L365) in that image.

The difference between `+load` and `__mod_init_func` is that the former guarantees [certain order of execution](https://developer.apple.com/documentation/objectivec/nsobject/1418815-load?language=objc), while the latter doesn't.

References:
* [Objective-C: What is a lazy class?](https://stackoverflow.com/a/15318325/3056242)
* [What did Runtime do during the startup of Runtime objc4-779.1 App?](https://programmer.group/what-did-runtime-do-during-the-startup-of-runtime-objc4-779.1-app.html)


