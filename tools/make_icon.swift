// Renders the DX11on10 app icon at the sizes Windows uses: swift tools/make_icon.swift <out-dir>
// Large sizes: "11" over a small "on 10" tag; small sizes: just "11".
import AppKit
import CoreGraphics

let out = CommandLine.arguments[1]
for size in [16, 20, 24, 32, 40, 48, 64, 96, 128, 256] {
    let s = CGFloat(size)
    let space = CGColorSpace(name: CGColorSpace.sRGB)!
    let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    let inset = s <= 32 ? 0.5 : s * 0.04
    let rect = CGRect(x: inset, y: inset, width: s - 2 * inset, height: s - 2 * inset)
    let path = CGPath(roundedRect: rect, cornerWidth: s * 0.22, cornerHeight: s * 0.22, transform: nil)
    ctx.addPath(path)
    ctx.clip()
    let gradient = CGGradient(colorsSpace: space, colors: [
        CGColor(srgbRed: 0.20, green: 0.45, blue: 1.00, alpha: 1),
        CGColor(srgbRed: 0.47, green: 0.27, blue: 0.98, alpha: 1)] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(gradient, start: CGPoint(x: 0, y: s), end: CGPoint(x: s, y: 0), options: [])
    // soft top highlight
    let shine = CGGradient(colorsSpace: space, colors: [
        CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 0.18), CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 0)] as CFArray,
        locations: [0, 1])!
    ctx.drawLinearGradient(shine, start: CGPoint(x: 0, y: s), end: CGPoint(x: 0, y: s * 0.45), options: [])
    ctx.resetClip()

    let ns = NSGraphicsContext(cgContext: ctx, flipped: false)
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = ns
    // Centres text visually: digits and lowercase without descenders sit on the baseline.
    func draw(_ text: String, _ font: NSFont, _ color: NSColor, centerY: CGFloat, kern: CGFloat = 0, xHeight: Bool = false) {
        let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: color, .kern: kern]
        let str = NSAttributedString(string: text, attributes: attrs)
        let size = str.size()
        let visible = xHeight ? max(font.xHeight, font.capHeight * 0.85) : font.capHeight
        str.draw(at: CGPoint(x: (s - size.width + kern) / 2, y: centerY - visible / 2 + font.descender))
    }
    if size <= 32 {
        draw("11", NSFont.systemFont(ofSize: s * 0.66, weight: .heavy), .white, centerY: s * 0.5, kern: -s * 0.02)
    } else {
        draw("11", NSFont.systemFont(ofSize: s * 0.56, weight: .heavy), .white, centerY: s * 0.61, kern: -s * 0.02)
        // "on 10" tag
        let tagH = s * 0.22, tagW = s * 0.56
        let tag = CGRect(x: (s - tagW) / 2, y: s * 0.13, width: tagW, height: tagH)
        ctx.setFillColor(CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 0.95))
        ctx.addPath(CGPath(roundedRect: tag, cornerWidth: tagH / 2, cornerHeight: tagH / 2, transform: nil))
        ctx.fillPath()
        draw("on 10", NSFont.systemFont(ofSize: s * 0.16, weight: .heavy),
             NSColor(srgbRed: 0.30, green: 0.33, blue: 0.95, alpha: 1), centerY: tag.midY)
    }
    NSGraphicsContext.restoreGraphicsState()
    let image = ctx.makeImage()!
    let rep = NSBitmapImageRep(cgImage: image)
    try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: "\(out)/icon_\(size).png"))
}
