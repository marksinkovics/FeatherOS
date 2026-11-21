import FeatherGraphics
import FeatherSupport
import GfxCheck

// Draws a picture with the graphics library from Swift and compares its checksum with the one
// gfx_check.cpp computes without the library, so no display is needed.

typealias Surface = FeatherOS.Graphics.Surface
typealias TileSheet = FeatherOS.Graphics.TileSheet
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

var sheet = TileSheet()
let status = sheet.Load(Check.TileRows(), Check.TileCount, Check.TileSize, Check.TilePalette(),
                        Check.PaletteSize)
print("gfxcheck: tiles \(sheet.TileCount()) of \(sheet.TileSize())x\(sheet.TileSize()), loaded \(status == .Ok)")
if status != .Ok {
    exit(.failure)
}
var picture = Surface(Check.TilePictureWidth, Check.TilePictureHeight)
picture.Fill(0x102030)
withUnsafeMutablePointer(to: &picture) { target in
    sheet.Draw(target, 0, 2, 2, .Lit, true)
    sheet.Draw(target, 0, 12, 2, .Remembered, true)
    sheet.Draw(target, 1, 22, 2, .Lit, true)
    sheet.Draw(target, 1, 32, 2, .Remembered, true)
    sheet.Draw(target, 1, -4, 12, .Lit, false)
    sheet.Draw(target, 0, 36, 16, .Remembered, true)
}
let tileActual = picture.Checksum()
let tileExpected = Check.ExpectedTileChecksum()
print("gfxcheck: tile checksum \(tileActual), match \(tileActual == tileExpected)")
if tileActual != tileExpected {
    print("gfxcheck: expected \(tileExpected)")
    exit(.failure)
}
