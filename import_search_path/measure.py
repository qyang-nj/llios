#!/usr/bin/env python3

import os
import sys
import json
from timeit import default_timer as timer

# Create and compile n modules.
# In the end, each module will have module{n}/Module{n}.swiftmodule,
# which will be imported later.
def create_modules(number_of_module):
    for i in range(number_of_module):
        module_dir = f"module{i}"
        if os.path.isdir(module_dir):
            continue

        os.mkdir(module_dir)
        source_file = f"{module_dir}/class{i}.swift"
        with open(source_file, "w") as f:
            f.write(f"public class Class{i}" + " {}")

        os.system("xcrun swiftc -c -parse-as-library -emit-module"
            + f" -module-name Module{i} -emit-module-path {module_dir}/Module{i}.swiftmodule"
            + f" {source_file}")

def create_vfsoverlay(import_path, number_of_search_path):
    swift_modules = list(map(lambda i: {
        "name": f"Module{i}.swiftmodule",
        "type": "file",
        "external-contents": f"module{i}/Module{i}.swiftmodule",
    }, range(number_of_search_path)))
    overlay = {
        "version": 0,
        "roots": [{
            "name": import_path,
            "type": "directory",
            "contents": swift_modules
        }]
    }
    return overlay

def measure_compile_time(number_of_search_path, number_of_import, use_vfsoverlay = False):
    create_modules(number_of_search_path)

    with open("main.swift", "w") as f:
        for i in range(number_of_import):
            f.write(f"import Module{i}\n")
        f.write('print("Hello, world!")\n')

    if use_vfsoverlay:
        overlay_file = "vfsoverlay.yaml"
        import_path = "/import"
        with open(overlay_file, "w") as outfile:
            overlay = create_vfsoverlay(import_path, number_of_search_path)
            json.dump(overlay, outfile)
        import_args = f"-Xfrontend -vfsoverlay -Xfrontend {overlay_file} -I /import"
    else:
        include_paths = ""
        for i in range(number_of_search_path):
            include_paths = f"-I module{i} " + include_paths
        import_args = include_paths

    start = timer()
    os.system(f"xcrun swiftc -c {import_args} main.swift")
    end = timer()
    return end - start


if __name__ == "__main__":
    use_vfsoverlay = "--use_vfsoverlay" in sys.argv

    for search_path_number in [0, 100, 500, 1000, 1500, 2000]:
        for import_number in [0, 100, 500, 1000, 1500, 2000]:
            if import_number > search_path_number:
                continue

            # Build once to warm up the cache (if there is any)
            measure_compile_time(search_path_number, import_number, use_vfsoverlay)

            total = 0
            for i in range(5):
                total += measure_compile_time(search_path_number, import_number, use_vfsoverlay)
            average = total / 5

            print("(include_path: {:4}, import: {:4}): {:2f}"
                .format(search_path_number, import_number, average))
