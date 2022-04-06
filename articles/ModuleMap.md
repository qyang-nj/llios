# Module Map
If you are working on iOS build system, it's almost impossible you haven't dealt with [modules](https://clang.llvm.org/docs/Modules.html#module-map-language). They not only provide [multiple advantages](https://clang.llvm.org/docs/Modules.html#semantic-import) over traditional headers but also play an essential role in the Objective-C and Swift interoperability. Module map is a file format that defines a module. We're going to explore some common module map formats.

Module maps are usually named as `module.modulemap`. If they're named that way, the compiler can load them from one of the search paths (`-I path/to/search`). Otherwise, we need to use `-fmodule-map-file=/absolute/path/to/MyModule.modulemap` to provide their exact locations.

## Module map with umbrella header
This is the most common form of module map.

```
module "MyModule" {
    umbrella header "MyModule.h"

    export *
    module * { export * }
}
```

The umbrella header is a file path that is relative to the directory where the module map is (not the working directory), so this can be `umbrella header "foo/MyModule.h"`, if the fire structure is like below. It’s can also be an absolute path, but really, who does that?
```
.
├── module.modulemap
└── foo
   └── MyModule.h
```

An umbrella header are likely to include other headers. Those headers can be located at one of the search paths that are provided to the compiler by `-I`. That means the other headers don't necessarily have to be in the same directory with the module map.


## Framework module map
Framework module maps have the keyword `framework`. They are usually inside of `.framework` bundles.
```
framework module "MyModule" {
    umbrella header "MyModule.h"

    export *
    module * { export * }
}
```

From [the official documentation](https://clang.llvm.org/docs/Modules.html#module-declaration):
> The framework qualifier specifies that this module corresponds to a Darwin-style framework. A Darwin-style framework (used primarily on macOS and iOS) is contained entirely in directory Name.framework, where Name is the name of the framework (and, therefore, the name of the module). That directory has the following layout:
> ```
> Name.framework/
>  Modules/module.modulemap  Module map **for** the framework
>  Headers/                  Subdirectory containing framework headers
>  PrivateHeaders/           Subdirectory containing framework private headers
>  Frameworks/               Subdirectory containing embedded frameworks
>  Resources/                Subdirectory containing additional resources
>  Name                      Symbolic link to the shared library **for** the framework
> ```

Unlike the non-framework module map, the framework module map assumes the umbrella header and all other headers are in the `Headers/` directory. If a header file is imported by the umbrella header but exists outside of the `Headers` directory, we will see a compiler error. To suppress the error, pass `-Wno-error=non-modular-include-in-framework-module` to `clang`.
> error: include of non-modular header inside framework module 'MyModule': ‘/path/to/other.h’

## Module map with individual headers
In stead of using an umbrella header, we can list individual headers in the module map.

```
module "MyModule" {
    export *

    header "foo.h"
    header "bar.h"
    header "dir/boo.h"
}
```


## Module map with umbrella directory
It's also possible to use an umbrella directory in lieu of an umbrella header.

```
module MyModule {
    umbrella "."

    export *
    module * { export * }
}
```

from [the official documentation](https://clang.llvm.org/docs/Modules.html#umbrella-directory-declaration)
> Umbrella directories are useful for libraries that have a large number of headers but do not have an umbrella header.

## Module map with auto linking
Auto linking can be applied when using module maps that use the link directive. For example with this module map file:
```
module "MyModule" {
    link “foo”
    link framework “Foundation”
}
```
It’s equivalent to adding `-lfoo` and `-framework Foundation` at linking time.

