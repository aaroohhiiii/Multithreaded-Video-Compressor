import AppKit
import PDFKit
import Foundation

guard CommandLine.arguments.count == 4 else {
    fputs("usage: extract INPUT.pdf OUTPUT.txt IMAGE_DIR\n", stderr)
    exit(2)
}

let input = CommandLine.arguments[1]
let textOutput = CommandLine.arguments[2]
let imageDirectory = CommandLine.arguments[3]

guard let document = PDFDocument(url: URL(fileURLWithPath: input)) else {
    fputs("could not open PDF\n", stderr)
    exit(1)
}

try FileManager.default.createDirectory(
    atPath: imageDirectory,
    withIntermediateDirectories: true
)

var completeText = ""
for pageIndex in 0..<document.pageCount {
    guard let page = document.page(at: pageIndex) else { continue }
    completeText += "\n===== PAGE \(pageIndex + 1) =====\n"
    completeText += page.string ?? "[NO EXTRACTABLE TEXT]"
    completeText += "\n"

    let thumbnail = page.thumbnail(
        of: NSSize(width: 1200, height: 1600),
        for: .mediaBox
    )
    guard let tiff = thumbnail.tiffRepresentation,
          let bitmap = NSBitmapImageRep(data: tiff),
          let png = bitmap.representation(using: .png, properties: [:]) else {
        fputs("could not render page \(pageIndex + 1)\n", stderr)
        exit(1)
    }
    let name = String(format: "page-%02d.png", pageIndex + 1)
    try png.write(to: URL(fileURLWithPath: imageDirectory).appendingPathComponent(name))
}

try completeText.write(
    toFile: textOutput,
    atomically: true,
    encoding: .utf8
)

print("pages=\(document.pageCount)")
