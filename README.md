# Low Level iOS
This repo is mostly my study notes about low level iOS.

If you find anything in my notes is incorrect, please create an [issue](https://github.com/qyang-nj/llios/issues)! If you have any questions, feel free to use [Discussions](https://github.com/qyang-nj/llios/discussions) so that we can learn something together.

## Topics
* [Mach-O Parser](./macho_parser) - writing a Mach-O format parser while learning it
    * [LC_SEGMENT_64](./macho_parser/docs/LC_SEGMENT_64.md) - segments and sections
    * [LC_SYMTAB](./macho_parser/docs/LC_SYMTAB.md) - symbol table
    * [LC_DYSYMTAB](./macho_parser/docs/LC_DYSYMTAB.md) - dynamic symbol table
    * [LC_DYLD_INFO(_ONLY)](./macho_parser/docs/LC_DYLD_INFO.md) - information for dyld
    * [LC_DYLD_CHAINED_FIXUPS](./dynamic_linking/chained_fixups.md) - the new way to encode dyld info
    * [LC_DYLD_EXPORTS_TRIE](./exported_symbol/README.md) - the new command for exported symbols
    * [LC_DYLD_ENVIRONMENT](./macho_parser/docs/LC_DYLD_ENVIRONMENT.md) - embed environment variables in the binary
    * [LC_LINKER_OPTION](./macho_parser/docs/LC_LINKER_OPTION.md) - auto linking
    * [LC_BUILD_VERSION](./macho_parser/docs/LC_BUILD_VERSION.md) - platform requirements
    * [LC_MAIN](./macho_parser/docs/LC_MAIN.md) - the entry point of an executable
    * [LC_ENCRYPTION_INFO_64](./macho_parser/docs/LC_ENCRYPTION_INFO.md) - FairPlay encryption
    * [LC_FUNCTION_STARTS](./macho_parser/docs/LC_FUNCTION_STARTS.md) - function addresses
    * [LC_*_DYLIB and LC_RPATH](./macho_parser/docs/LC_dylib.md) - dylib related load commands
    * [LC_CODE_SIGNATURE](./macho_parser/docs/LC_CODE_SIGNATURE.md) - code signing and code signature format
* Building
    * [Build iOS App](./build_ios_app) - build and debug an iOS app without an IDE
    * [Mixed Language Compiling](./articles/MixedModuleCompiling.md)
    * [Module Map](./articles/ModuleMap.md) - common formats of `module.modulemap`
    * [Import Search Path](./import_search_path) - how import search path (`-I`) affects build time
    * [Swift Generated ObjC Header](./building/swift_generated_objc_header/README.md)
    * [Dead Code Elimination](./dce)
* Dynamic Linking
    * [How does dynamic linking works](./dynamic_linking)
    * [Exported Symbols](./exported_symbol/) - details of how exported symbols are stored
    * [Binding Info](./dynamic_linking/docs/BindingInfo.md) - details of binding opcodes
    * [Chained Fixups](./dynamic_linking/chained_fixups.md) - the new way to store dyld info
    * [Dynamic Interposing](./dynamic_linking/dynamic_interposing.md) - replace function implementation at runtime
* Testing
    * [Behind the scenes: iOS Testing](./articles/iOSTesting.md)
    * [Archived] [XCTest](./xctest)
* Xcode
    * [Behind the scenes: SwiftUI Previews](./articles/SwiftUIPreview.md)
    * [Xcode.app Directory Structure](./articles/XcodeDirectoryStructure.md)
