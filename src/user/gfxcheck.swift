import FeatherGraphics
import FeatherSupport
import GfxCheck

// Draws a picture with the graphics library from Swift and compares its checksum with the one
// gfx_check.cpp computes without the library, so no display is needed.

typealias Surface = FeatherOS.Graphics.Surface
typealias Check = FeatherOS.User.GfxCheck

var surface = Surface(Check.Width, Check.Height)
surface.Fill(0x101820)
surface.FillRectangle(4, 4, 30, 20, 0xC03030)
surface.DrawRectangle(-2, -2, 20, 12, 0xFFFFFF)
surface.DrawText(6, 6, "Feather", 0xFFFF00, 1)
surface.DrawText(20, 24, "OS", 0x00FF00, 2)

var sprite = Surface(8, 8)
sprite.Fill(0xFF00FF)
sprite.FillRectangle(2, 2, 4, 4, 0x00FFFF)
withUnsafePointer(to: &sprite) { surface.BlitKeyed($0, 50, 30, 0xFF00FF) }

let actual = surface.Checksum()
let expected = Check.ExpectedChecksum()
print("gfxcheck: surface \(surface.Width())x\(surface.Height()), sprite \(sprite.Width())x\(sprite.Height())")
print("gfxcheck: checksum \(actual), match \(actual == expected)")
if actual != expected {
    print("gfxcheck: expected \(expected)")
    exit(.failure)
}
