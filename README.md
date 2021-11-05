# Lower Level iOS
Random stuff about lower level iOS

## Topics
* [Mach-O Parser](./macho_parser) - writing a Mach-O format parser while learning it
    * [LC_SYMTAB](./macho_parser/docs/LC_SYMTAB.md) - symbol table
    * [LC_DYSYMTAB](./macho_parser/docs/LC_DYSYMTAB.md) - dynamic symbol table
    * [LC_CODE_SIGNATURE](./macho_parser/docs/LC_CODE_SIGNATURE.md) - code signing and code signature format
* Dynamic Linking
    * [How does dynamic linking works](./dynamic_linking)
    * [Exported Symbols](./exported_symbol/) - details of how exported symbols are stored
    * [Chained Fixups](./dynamic_linking/chained_fixups.md) - the new way to store dyld info
* [Build iOS App](./build_ios_app) - build and debug an iOS app without an IDE
* [Dead Code Elimination](./dce)
* [XCTest](./xctest) - bare-bones of iOS testing
* [Import Search Path](./import_search_path) - how import search path (`-I`) affects build time
