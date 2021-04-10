SDK = iphonesimulator
SDK_PATH = $(shell xcrun --show-sdk-path --sdk $(SDK))
# Targeting ios12 or lower will end up linking with libSwiftCompatibility50 and libSwiftCompatibilityDynamicReplacements.
TARGET = x86_64-apple-ios12.0-simulator
SWIFTC = xcrun swiftc -sdk $(SDK_PATH) -target $(TARGET)
APP_NAME = SimpleApp

# Build Swift static library.
define OUTPUT_FILE_MAP
{
  "": {"swift-dependencies": "Build/StaticLib-master.swiftdeps"},
  "Sources/StaticLib/foo.swift": {"object": "Build/foo.o"},
  "Sources/StaticLib/bar.swift": {"object": "Build/bar.o"}
}
endef

export OUTPUT_FILE_MAP
static_lib: OUTPUT_FILE_MAP_JSON = Build/StaticLib_OutputFileMap.json
static_lib:
	@mkdir -p Build
	@echo "$$OUTPUT_FILE_MAP" > $(OUTPUT_FILE_MAP_JSON)
# -parse-as-library: Parse the input file(s) as libraries, not scripts. Without this, swiftc will include main, which causes duplicate symbols.
# -c (-emit-object)
	@$(SWIFTC) \
		-incremental \
		-parse-as-library \
		-c \
		-module-name StaticLib \
		-emit-module \
		-emit-module-path Build/StaticLib.swiftmodule \
		-index-store-path Build/IndexStore \
		-output-file-map $(OUTPUT_FILE_MAP_JSON) \
		Sources/StaticLib/bar.swift Sources/StaticLib/foo.swift

	@xcrun libtool \
		-static \
		-o Build/StaticLib.a \
		Build/foo.o Build/bar.o

dynamic_lib:
	@mkdir -p Build
# -emit-library: Genraete dynamic library
	@$(SWIFTC) \
		-emit-library \
		-emit-module \
		-o Build/DynamicLib.dylib \
		-module-name DynamicLib \
		-emit-module-path Build/DynamicLib.swiftmodule \
		-Xlinker -install_name -Xlinker @rpath/DynamicLib.dylib \
		Sources/DynamicLib/DynamicLib.swift

# Build Swift executable linking with a static library.
executable: static_lib dynamic_lib

# -rpath: Add search path for @rpath placeholder
	@$(SWIFTC) \
		-emit-executable \
		-I Build \
		-o Build/$(APP_NAME) \
		-Xlinker -rpath -Xlinker @executable_path/ \
		Build/StaticLib.a Build/DynamicLib.dylib \
		Sources/AppDelegate.swift Sources/ViewController.swift Sources/SwiftUIView.swift

# Process Info.plist
PLIST_BUDDY = /usr/libexec/PlistBuddy
process_plist:
	@cp Sources/Info.plist Build/Info.plist
	@$(PLIST_BUDDY) -c "Set :CFBundleDevelopmentRegion en" Build/Info.plist
	@$(PLIST_BUDDY) -c "Set :CFBundleExecutable $(APP_NAME)" Build/Info.plist
	@$(PLIST_BUDDY) -c "Set :CFBundleName $(APP_NAME)" Build/Info.plist
	@$(PLIST_BUDDY) -c "Set :CFBundleIdentifier com.qyang-nj.$(APP_NAME)" Build/Info.plist
	@$(PLIST_BUDDY) -c "Set :CFBundlePackageType APPL" Build/Info.plist

# Build app bundle
app: executable process_plist
	@mkdir -p Build/$(APP_NAME).app
	@mv Build/$(APP_NAME) Build/$(APP_NAME).app
	@mv Build/DynamicLib.dylib Build/$(APP_NAME).app
	@mv Build/Info.plist Build/$(APP_NAME).app

	@echo "Done. Build/$(APP_NAME).app"

# Launch app in the simulator
launch_app: app
	@xcrun simctl shutdown all
	@xcrun simctl boot "iPhone 8"
	@xcrun simctl install booted Build/$(APP_NAME).app
	@xcrun simctl launch booted com.qyang-nj.$(APP_NAME)

clean:
	@rm -rf Build
