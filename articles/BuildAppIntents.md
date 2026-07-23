# Build App Intents

App Intents are declared in Swift, but compiling the Swift source is only the first part of the build. Xcode also extracts the declarations into metadata and generates natural-language assets for App Shortcuts. The final app bundle contains these generated files in `Metadata.appintents`.

This article describes the build pipeline used by Xcode 26.6, which is useful when reproducing Xcode's behavior in another build system. The captured commands use tool build `17F113` and the iPhone Simulator 26.5 SDK. These tools, flags, and output formats are Xcode implementation details and may change between releases.

The linked [`AppShortcuts.swift`](../building/app_intents/AppShortcuts.swift) sample defines two intents and exposes them through an `AppShortcutsProvider`. The build has three stages:

1. Compile the Swift sources and emit constant-value sidecar files.
2. Extract App Intents metadata from those sidecar files.
3. Generate and archive the App Shortcuts natural-language assets.

## 1. Emit Swift constant values

For each Swift compilation job that contains App Intents declarations, pass `-emit-const-values-path` to the Swift frontend:

```text
-emit-const-values-path {Objects-normal}/AppShortcuts.swiftconstvalues
```

The resulting `.swiftconstvalues` file is JSON. It records the intent types, protocol conformances, parameters, titles, descriptions, shortcut phrases, and other values that can be evaluated at compile time. See the sample [`AppShortcuts.swiftconstvalues`](../building/app_intents/AppShortcuts.swiftconstvalues); its machine-specific source paths have been replaced with the `{PROJECT_DIR}` placeholder.

Next, write a `*.SwiftConstValuesFileList` containing the absolute path of every `.swiftconstvalues` file in the target, one path per line. Xcode names this file after the module; for example:

```text
{Objects-normal}/AppShortcuts.swiftconstvalues
{Objects-normal}/AnotherIntent.swiftconstvalues
```

The metadata extractor consumes this file list rather than receiving each sidecar path separately.

## 2. Extract App Intents metadata

Xcode runs `appintentsmetadataprocessor` in the `ExtractAppIntentsMetadata` build step. **Extracting App Intents metadata requires the app binary, so this step runs after the app binary is linked.** The following command shows the important inputs from the captured build; paths use `{DEVELOPER_DIR}` and `{DerivedData}` placeholders:

```shell
{DEVELOPER_DIR}/Toolchains/XcodeDefault.xctoolchain/usr/bin/appintentsmetadataprocessor \
  --toolchain-dir {DEVELOPER_DIR}/Toolchains/XcodeDefault.xctoolchain \
  --module-name AppDemo \
  --sdk-root {DEVELOPER_DIR}/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator26.5.sdk \
  --xcode-version 17F113 \
  --platform-family iOS \
  --deployment-target 26.5 \
  --bundle-identifier com.example.AppDemo \
  --output {DerivedData}/Build/Products/Debug-iphonesimulator/AppDemo.app \
  --target-triple arm64-apple-ios26.5-simulator \
  --binary-file {DerivedData}/Build/Products/Debug-iphonesimulator/AppDemo.app/AppDemo \
  --dependency-file {DerivedData}/Build/Intermediates.noindex/AppDemo.build/Debug-iphonesimulator/AppDemo.build/Objects-normal/arm64/AppDemo_dependency_info.dat \
  --stringsdata-file {DerivedData}/Build/Intermediates.noindex/AppDemo.build/Debug-iphonesimulator/AppDemo.build/Objects-normal/arm64/ExtractedAppShortcutsMetadata.stringsdata \
  --source-file-list {DerivedData}/Build/Intermediates.noindex/AppDemo.build/Debug-iphonesimulator/AppDemo.build/Objects-normal/arm64/AppDemo.SwiftFileList \
  --metadata-file-list {DerivedData}/Build/Intermediates.noindex/AppDemo.build/Debug-iphonesimulator/AppDemo.build/AppDemo.DependencyMetadataFileList \
  --static-metadata-file-list {DerivedData}/Build/Intermediates.noindex/AppDemo.build/Debug-iphonesimulator/AppDemo.build/AppDemo.DependencyStaticMetadataFileList \
  --swift-const-vals-list {DerivedData}/Build/Intermediates.noindex/AppDemo.build/Debug-iphonesimulator/AppDemo.build/Objects-normal/arm64/AppDemo.SwiftConstValuesFileList \
  --compile-time-extraction \
  --deployment-aware-processing \
  --validate-assistant-intents \
  --no-app-shortcuts-localization
```

The extractor combines the target's constant values with metadata from its dependencies. It writes [`Metadata.appintents`](../building/app_intents/Metadata.appintents/) into the app bundle. In the sample, [`extract.actionsdata`](../building/app_intents/Metadata.appintents/extract.actionsdata) contains the serialized definitions for both intents and their shortcuts, while [`version.json`](../building/app_intents/Metadata.appintents/version.json) identifies the format and tool version.

## 3. Generate App Shortcuts assets

If the target provides App Shortcuts, Xcode follows metadata extraction with the `AppIntentsSSUTraining` step. It runs `appintentsnltrainingprocessor` to turn the shortcut phrases and localized application name into natural-language assets:

```shell
{DEVELOPER_DIR}/Toolchains/XcodeDefault.xctoolchain/usr/bin/appintentsnltrainingprocessor \
  --infoplist-path {DerivedData}/Build/Products/Debug-iphonesimulator/AppDemo.app/Info.plist \
  --temp-dir-path {DerivedData}/Build/Intermediates.noindex/AppDemo.build/Debug-iphonesimulator/AppDemo.build/ssu \
  --bundle-id com.example.AppDemo \
  --product-path {DerivedData}/Build/Products/Debug-iphonesimulator/AppDemo.app \
  --extracted-metadata-path {DerivedData}/Build/Products/Debug-iphonesimulator/AppDemo.app/Metadata.appintents \
  --metadata-file-list {DerivedData}/Build/Intermediates.noindex/AppDemo.build/Debug-iphonesimulator/AppDemo.build/AppDemo.DependencyMetadataFileList \
  --source-file {DerivedData}/Build/Products/Debug-iphonesimulator/AppDemo.app/Info.plist \
  --archive-ssu-assets
```

It first generates [`root.ssu.yaml`](../building/app_intents/Metadata.appintents/root.ssu.yaml), which maps each intent to its localized training utterances. With `--archive-ssu-assets`, it also writes the compressed [`nlu.lzfse`](../building/app_intents/Metadata.appintents/nlu/nlu.lzfse) archive and its [version file](../building/app_intents/Metadata.appintents/nlu/b6dd1cdf99351cdd4bea3e8516c51211.version) into the app bundle.

## Final app bundle

After both processors run, the relevant part of the app bundle looks like this:

```text
AppDemo.app/
└── Metadata.appintents/
    ├── extract.actionsdata
    ├── nlu/                       (not present in the Bazel build)
    │   ├── b6dd1cdf99351cdd4bea3e8516c51211.version
    │   └── nlu.lzfse
    ├── root.ssu.yaml              (not present in the Bazel build)
    └── version.json
```

The Bazel build examined here stops after `appintentsmetadataprocessor`, so it produces `extract.actionsdata` and `version.json` but not `root.ssu.yaml` or the `nlu` directory created by the training step.
