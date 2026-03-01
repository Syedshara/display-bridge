// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "DisplayBridgeReceiver",
    platforms: [.macOS(.v13)],
    targets: [
        .systemLibrary(
            name: "CSRT",
            path: "Sources/CSRT",
            pkgConfig: "srt",
            providers: [
                .brew(["srt"])
            ]
        ),
        .executableTarget(
            name: "DisplayBridgeReceiver",
            dependencies: ["CSRT"],
            path: "Sources/DisplayBridgeReceiver",
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("Metal"),
                .linkedFramework("MetalKit"),
                .linkedFramework("Network"),
                .linkedFramework("VideoToolbox"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("IOSurface"),
            ]
        )
    ]
)
