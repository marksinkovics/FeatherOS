import FeatherGraphics
import FeatherSupport
import GfxCheck

// Draws a picture with the graphics library from Swift and compares its checksum with the one
// gfx_check.cpp computes without the library, so no display is needed.

typealias Surface = FeatherOS.Graphics.Surface
typealias TileSheet = FeatherOS.Graphics.TileSheet
typealias Font = FeatherOS.Graphics.Font
typealias Check = FeatherOS.User.GfxCheck

func loadFont(_ name: String) -> UnsafeMutablePointer<Font> {
    let font = UnsafeMutablePointer<Font>.allocate(capacity: 1)
    font.initialize(to: Font())
    let status = font.pointee.LoadNamed(name)
    if status != .Ok {
        print("gfxcheck: font \(name) rejected \(String(cString: FeatherOS.Text.FontStatusName(status)))")
        exit(.failure)
    }
    return font
}

let sansPath = "/system/fonts/sans-16.ffnt"
let monoPath = "/system/fonts/mono-32.ffnt"
let sans = loadFont("sans-16")
let mono = loadFont("mono-32")

var surface = Surface(Check.Width, Check.Height)
surface.Fill(0x101820)
surface.FillRectangle(4, 4, 30, 20, 0xC03030)
surface.DrawRectangle(-2, -2, 20, 12, 0xFFFFFF)
_ = surface.DrawText(sans, 6, 6, "Feather", 0xFFFF00)
_ = surface.DrawText(mono, 12, 22, "OS", 0x00FF00)

var sprite = Surface(8, 8)
sprite.Fill(0xFF00FF)
sprite.FillRectangle(2, 2, 4, 4, 0x00FFFF)
withUnsafePointer(to: &sprite) { surface.BlitKeyed($0, 50, 30, 0xFF00FF) }

let actual = surface.Checksum()
let expected = Check.ExpectedChecksum(sansPath, monoPath)
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

var textPicture = Surface(Check.FontPictureWidth, Check.FontPictureHeight)
textPicture.Fill(0x203050)
let textEnd = textPicture.DrawText(sans, 4, 4, Check.FontText(), 0xF0E0C0)
_ = textPicture.DrawTextOn(sans, 4, 26, Check.OnText(), 0x00FF00, 0x602020)
guard let console = Font.Console() else {
    print("gfxcheck: no console font")
    exit(.failure)
}
_ = textPicture.DrawText(console, 50, 26, Check.ConsoleText(), 0xFFFF00)
_ = textPicture.DrawText(console, 70, 40, "cut", 0xFF8080)
let fontActual = textPicture.Checksum()
let fontExpected = Check.ExpectedFontChecksum(sansPath)
print("gfxcheck: font checksum \(fontActual), match \(fontActual == fontExpected)")
if fontActual != fontExpected {
    print("gfxcheck: expected \(fontExpected)")
    exit(.failure)
}
print("gfxcheck: font \(String(cString: sans.pointee.Name())), line \(sans.pointee.LineHeight()), text ends at \(textEnd), console \(console.pointee.CellWidth())x\(console.pointee.LineHeight())")
