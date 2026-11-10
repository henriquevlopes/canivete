// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "CaniveteMac",
    platforms: [.macOS(.v13)],
    products: [.executable(name: "Canivete", targets: ["CaniveteMac"])],
    targets: [
        .executableTarget(name: "CaniveteMac", path: "Sources/CaniveteMac")
    ]
)
