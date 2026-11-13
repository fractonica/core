// swift-tools-version:5.7
//
// fractonica-core as a Swift package: the C99 core exposed to Swift as the
// module `CFractonica` (import CFractonica).
//
// The target is the repository itself: sources are src/*.c, the public header
// is include/fractonica.h. SwiftPM generates the module map from the include
// directory, so there is no module.modulemap to maintain.
//
// A consumer (the iOS / macOS app) depends on it by path or by URL:
//
//     .package(path: "../core")
//     .target(name: "App", dependencies: [.product(name: "CFractonica", package: "core")])
//
// The package identity is the directory (or repository) name, "core".
// See docs/GUIDE.md, "Apple platforms", for what Swift sees of the C API.

import PackageDescription

let package = Package(
    name: "FractonicaCore",
    platforms: [
        .iOS(.v16),
        .macOS(.v13),
        .watchOS(.v9),
        .tvOS(.v16),
    ],
    products: [
        .library(name: "CFractonica", targets: ["CFractonica"]),
    ],
    targets: [
        .target(
            name: "CFractonica",
            path: ".",
            sources: ["src"],
            publicHeadersPath: "include"
        ),
    ],
    cLanguageStandard: .c99
)
