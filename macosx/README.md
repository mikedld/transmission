## macOS project for Transmission

This folder contains two deliverables:
  * the Transmission app for macOS
  * the QuickLookPlugin for macOS

## Minimum supported versions

### macOS version

Transmission 4 has its minimum macOS version as **macOS 10.13**.

### Xcode version

Transmission has its minimum Xcode version as **Xcode 11.3.1**.
=> the project needs to be retrocompatible **Xcode 11.3.1** and newer.

Motivation: This is the latest installable Xcode for macOS 10.14.6, which itself is the RECOMMENDED_MACOSX_DEPLOYMENT_TARGET of latest stable release (Xcode 14.2).
(see https://xcodereleases.com)

### Swift version

Transmission has its minimum Swift version as **Swift 5.1.3**.
=> the project needs to be retrocompatible **Swift 5.1.3** and newer.

Motivation: This is the Swift version bundled with Xcode 11.3.1.
(see https://xcodereleases.com)

## Contributing

### Retro-compatibility

In Swift, whenever you need to use `if #available` syntax, you should encapsulate it with `#if compiler` with the min Swift version that supports it. Example:
```
#if compiler(>=5.3.1)
    if #available(macOS 11.0, *) {
        return NSImage(systemSymbolName: symbolName, accessibilityDescription: nil)
    }
#endif
```     

In ObjC, whenever you need to use `if (@available)` syntax, you should encapsulate it with `#ifdef __MAC_` with the min macOS version that supports it. Example:
```
#ifdef __MAC_11_0
    if (@available(macOS 11.0, *))
    {
        return [NSImage imageWithSystemSymbolName:symbolName accessibilityDescription:nil];
    }
#endif
```

### Code Style

`brew install swiftlint`

### Bridging libtransmission to Swift

Add what you need to expose to Swift to the `objc_*.h` files, with the added prefix `c_` or as an ObjC class, and implement the bridge in the `objc_*.mm` files.
