// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "EarCue",
    platforms: [
        .macOS(.v14)
    ],
    products: [
        .executable(name: "EarCue", targets: ["EarCue"])
    ],
    targets: [
        .target(
            name: "EarCueAudio",
            path: "Sources/EarCueAudio",
            publicHeadersPath: "include",
            linkerSettings: [
                .linkedFramework("CoreAudio"),
                .linkedFramework("AudioToolbox"),
                .linkedFramework("CoreFoundation")
            ]
        ),
        .executableTarget(
            name: "EarCue",
            dependencies: ["EarCueAudio"],
            path: "Sources/EarCue",
            linkerSettings: [
                .linkedFramework("Carbon"),
                .linkedFramework("CoreAudio")
            ]
        )
    ]
)
