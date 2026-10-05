# CursorMotionBlur

A tiny Windows tray app that adds a **live motion blur to your mouse cursor**, system-wide, like macOS.

![Demo: the cursor leaves a fading blur trail that gets longer as it speeds up, and the real cursor disappears at the end when it's very fast](assets/demo.gif)

*(Recorded with strength and trail length turned up so it's easy to see. The defaults are subtler, and you can tune them live in Settings.)*

- Fading blur trail behind the cursor, drawn in real time at your mouse's polling rate
- Optionally hides the real cursor when you move very fast, so only the blur remains
- Works across multiple monitors with different resolutions and display scaling
- Lightweight: a single ~35 KB `.exe`, no installer, no runtime to install (uses the .NET Framework already in Windows 10/11), about 1% of one CPU core while idle

## Use it

1. Download `CursorMotionBlur.exe` from the [Releases](../../releases) page and run it.
2. A settings window opens on first run. Everything applies instantly, so tune it by moving the mouse.
3. It lives in the system tray. Right-click the icon for **Enabled / Settings / Exit**, or press **Ctrl+Alt+B** anywhere to toggle it.

| Setting | What it does |
| --- | --- |
| Blur strength | Peak opacity of the blur (1-100%) |
| Trail length | How far back in time the blur reaches (10-150 ms) |
| Hide the real cursor when moving very fast | Swaps the system cursors for an invisible one above the chosen speed, then restores them |
| Launch when Windows starts | Adds or removes a per-user startup entry |

Settings are stored in `%APPDATA%\CursorMotionBlur\settings.ini`.

### If the cursor ever stays invisible

CursorMotionBlur restores your cursors on exit, on crash and on the next start. If something still goes wrong, just run `CursorMotionBlur.exe` again, or open *Mouse > Pointers* in Windows settings and click OK.

## Build from source

Needs only Windows 10/11 (the C# compiler ships with Windows):

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

The result is `dist\CursorMotionBlur.exe`. `tools\make-icon.ps1` regenerates `assets\icon.ico`.

## How it works

A layered, click-through, always-on-top window draws fading copies of the current cursor image along its recent path. Cursor positions are sampled at about 500 Hz, and a frame is drawn for every new mouse position. For the "hide when fast" option the app replaces the static system cursors using `SetSystemCursor` and restores them with `SystemParametersInfo(SPI_SETCURSORS)`.

## License

Public domain ([Unlicense](LICENSE)). Anyone can use, copy, modify or sell it, no strings attached.

