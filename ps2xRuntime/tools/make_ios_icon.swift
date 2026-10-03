// ICO1: iOS app icon = the Android launcher mountain (LB1 ic_ssx_*_fg.xml) without the 60/120 badge.
// Usage: swift ps2xRuntime/tools/make_ios_icon.swift ps2xRuntime/ios/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png   (1024x1024, opaque, sRGB)
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let size = 1024
let cs = CGColorSpace(name: CGColorSpace.sRGB)!
let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0,
                    space: cs, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
func rgb(_ hex: UInt32) -> CGColor {
    CGColor(colorSpace: cs, components: [CGFloat((hex >> 16) & 0xff) / 255, CGFloat((hex >> 8) & 0xff) / 255,
                                         CGFloat(hex & 0xff) / 255, 1])!
}
// Background (ic_ssx_120_bg: #D9541E).
ctx.setFillColor(rgb(0xD9541E)); ctx.fill(CGRect(x: 0, y: 0, width: size, height: size))
// Android viewport is 108x108 with y down; the mountain spans x 22..86, y 30..70 (centre 54,50).
// Scale it up ~1.3x around its centre and place that centre at the canvas centre.
let s = CGFloat(size) / 108 * 1.3
func p(_ x: CGFloat, _ y: CGFloat) -> CGPoint {
    CGPoint(x: CGFloat(size) / 2 + (x - 54) * s, y: CGFloat(size) / 2 - (y - 50) * s)  // flip y for CG
}
func poly(_ pts: [(CGFloat, CGFloat)], _ color: UInt32) {
    ctx.beginPath(); ctx.move(to: p(pts[0].0, pts[0].1))
    for q in pts.dropFirst() { ctx.addLine(to: p(q.0, q.1)) }
    ctx.closePath(); ctx.setFillColor(rgb(color)); ctx.fillPath()
}
poly([(22, 70), (46, 30), (58, 50), (64, 42), (86, 70)], 0xFFFFFF)   // snow mountain
poly([(46, 30), (58, 50), (50, 56), (44, 48), (40, 54)], 0xB8D4F0)   // shaded face
let img = ctx.makeImage()!
let url = URL(fileURLWithPath: CommandLine.arguments[1])
let dst = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
CGImageDestinationAddImage(dst, img, nil)
precondition(CGImageDestinationFinalize(dst))
