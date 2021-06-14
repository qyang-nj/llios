# Import Search Path
In this article, we are going to investigate how build time is impacted by the number of import and the number of import search path (`-I`). Also, we will take a look at VFS overlays and see how it can improve the build time.

## Basics
If you import a module, e.g. `import Foo`, in a swift file, the swift compiler needs to locate that module's swiftmodule (`Foo.swiftmodule`) file in one of the import search paths, which are specified via `-I` flag. This is very similar to how a C compiler finds the included header files.
```
swiftc -I path1 -I path2 source.swift
```

## Measurement
First, we generate and compile enough modules that will be imported later. Each module is in its own directory and just has an empty class. In the end of this step, we will have, say 2000, modules defined in `module{n}/Module{n}.swiftmodule`.
```
$ cat module66/class66.swift
public class Class66 {}

$ swiftc -c -parse-as-library -emit-module -module-name Module66 \
    -emit-module-path module66/Module66.swiftmodule module0.swift

$ ls module66
class66.swift  Module66.swiftdoc  Module66.swiftmodule  Module66.swiftsourceinfo
```

Second, we generate a swift file (`main.swift`) with x imports. We are only importing those modules and not using the class from them. This avoids increasing the code complexity, which is another dimension that can affect build time.
```swift
// main.swift
import Module0
import Module1
...
import Module99
print("Hello, world!")
```
Third, we compile this swift file with y import search paths. We are just emitting the object file (`-c`) to avoid the cost of linking.
```
swiftc -c -I module0 -I module1 ... -I module99 main.swift
```

Lastly, we measure the build time against various number of import and search path, repeat multiple times, and calculate the average. The full script is [here](./measure.py).

## Result
I compiled the result in the table below. The top row is the number of import search path and the left column is the number of import in the swift file. The unit of data is second. All the measurements are done by Swift 5.4 / XCode 12.5.

|    |0       |100     |500     |1000     |1500     |2000     |
|----|--------|--------|--------|---------|---------|---------|
|0   |0.048533|0.065908|0.119099|0.176846 |0.318999 |0.331329 |
|100 |        |0.184126|1.348830|2.477664 |4.088415 |5.543565 |
|500 |        |        |3.447714|10.683562|17.890379|23.217702|
|1000|        |        |        |14.103063|28.380111|40.873390|
|1500|        |        |        |         |30.438282|50.331757|
|2000|        |        |        |         |         |54.957123|

As we can see here the time complexity is approximately **O(m*n)**,  where **m** is the number of search path (`-I`) and **n** is the number of import.


## VFS Overlay
If we use `time` command to time `swiftc` with 1000 modules, we can see the majority of time are spent on system calls, which, as my guess, is caused by the compiler traversing all search paths in real file system.
```
real    0m11.778s
user    0m1.545s
sys     0m10.164s
```

Instead of going through the real file system, LLVM provides a virtual file system mechanism. However, I hardly find any official documentation other than the [source code](https://github.com/llvm/llvm-project/blob/llvmorg-12.0.0/llvm/include/llvm/Support/VirtualFileSystem.h#L522). My understanding is that the VFS overlay is just a yaml config file, which basically is a map. The compiler can locate the swiftmodule file in the map and read it directly from the real file system. Thus traversing search directories is avoided.

``` yaml
# vfsoverlay.yaml
{
    "version": 0,
    "roots": [{
        "name": "/import",
        "type": "directory",
        "contents": [{
            "name": "Module0.swiftmodule"
            "type": "file",
            "external-contents": "module0/Module0.swiftmodule" },
            ...
        ]
    }]
}
```

After generating the overlay file, we can, instead of using a number of `-I`s, just provide the overlay file and pass a single `-I`.

```
swift -Xfrontend -vfsoverlay -Xfrontend vfsoverlay.yaml -I /import ...
```


We then run the same measurement. Below shows the result. Same as previous table, the top row is the number of import search path and the left column is the number of import in the swift file.
|    |0       |100     |500     |1000    |1500    |2000    |
|----|--------|--------|--------|--------|--------|--------|
|0   |0.047331|0.054232|0.059363|0.061992|0.080064|0.100860|
|100 |        |0.088106|0.094788|0.103275|0.125440|0.149046|
|500 |        |        |0.241679|0.279762|0.313207|0.358585|
|1000|        |        |        |0.506527|0.562401|0.672119|
|1500|        |        |        |        |0.820307|0.954050|
|2000|        |        |        |        |        |1.271511|

It's obvious that using VFS overlay significantly reduces the build time. Although generating the overlay file takes a little extra time, but it's totally worth it, especially for a large number of imports.

## Learn more
* [llvm/include/llvm/Support/VirtualFileSystem.h](https://github.com/llvm/llvm-project/blob/llvmorg-12.0.0/llvm/include/llvm/Support/VirtualFileSystem.h)
* [Swift, Module Maps & VFS Overlays: Example](https://github.com/milend/swift-vfs-overlay-module-map-example)
* [Add vfsoverlay feature for improved compilation performance](https://github.com/bazelbuild/rules_swift/pull/375)
