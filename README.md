# NativeVideo for LOVE

Play MP4 (and whatever else the OS can decode) in LOVE 11.x or LOVE 12.0 using the operating system's own video decoders. No FFmpeg to bundle, no codec licences.

| OS | API | Typical formats |
|---|---|---|
| Windows | Media Foundation | MP4/MOV (H.264), WebM (VP9); HEVC/AV1 with the free Store extensions |
| macOS | AVFoundation | MP4/MOV (H.264, HEVC, ProRes) |
| Linux | GStreamer | Whatever plugins are installed (`gstreamer1.0-libav` covers almost everything) |

## Files

- `NativeVideo.lua` - The player (copy into your game).
- `nativevideo.dll` - Prebuilt Windows x64 library.
- `libnativevideo.so` - Prebuilt Linux x64 library (needs GStreamer 1.x installed).
- `main.lua`, `conf.lua` - Demo: drag a video onto the window.
- `src/` - C sources: one shared API, one file per OS.
- `test/` - Automated tests and test videos.
- `CMakeLists.txt` - Builds the library for the current OS.

## Use it

Put `NativeVideo.lua` and the library for your platform next to `main.lua` (for a packaged game: next to the `.love`/`.exe`).

```lua
local NativeVideo = require("NativeVideo")
local video

function love.load()
    video = NativeVideo.new("videos/intro.mp4") -- LOVE path or absolute path
    video:setLoop(true)
    video:play()
end

function love.update(dt)
    video:update(dt)
end

function love.draw()
    video:draw(0, 0) -- Same arguments as love.graphics.draw
end
```

### Syncing to music (rhythm games)

Don't call `update(dt)`. Drive the video from the audio clock instead:

```lua
function love.update(dt)
    video:setTime(music:tell())
end
```

Small forward steps decode normally. Going backwards or jumping more than 1 second seeks, and the exact frame for that time is shown.

## API

| Call | Description |
|---|---|
| `NativeVideo.new(path)` | Open; errors with a readable message on failure. |
| `:play()`, `:pause()`, `:stop()`, `:isPlaying()` | Playback controls and playback state. |
| `:update(dt)` | Advance by `dt` (uses `:setSpeed(s)`). |
| `:seek(t)` | Jump to the exact frame at `t` seconds. |
| `:setTime(t)` | Follow an external clock. |
| `:tell()`, `:getDuration()`, `:getFPS()` | Get playback time, duration, and frames per second. |
| `:setLoop(bool)`, `:isLooping()` | Set or check looping. |
| `:getDimensions()`, `:getImage()` | Get video dimensions or a normal LOVE `Image` (usable with shaders, quads, meshes, etc.). |
| `:draw(x, y, r, sx, sy, ox, oy)` | Draw the video. |
| `:release()` | Free resources now (otherwise freed by GC). |
| `NativeVideo.getBackend()` | Returns `"mediafoundation"`, `"avfoundation"`, or `"gstreamer"`. |

## How it works

- `src/nativevideo.h` is the whole C API: `nv_open`, `nv_read_frame`, `nv_seek`, and `nv_close`. Every backend outputs plain RGBA, so Lua doesn't care which OS it's on.
- The decoder writes straight into an `ImageData`'s memory (`getFFIPointer`). There is no copy through Lua strings.
- Two frames are held: the one on screen and the next one waiting for its timestamp. If a frame takes too long (lag spike), the player decodes forward but uploads only the newest frame to the GPU, so it catches up instead of drifting.
- Timestamps are normalised so the first frame is always 0.0s. MP4s with B-frames otherwise start at approximately 0.067s on Windows.
- Seek lands on the exact frame on every backend. If the OS seek overshoots, it retries a little earlier.

## Building

The prebuilt Windows and Linux libraries are included. To build yourself:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The build copies the library next to `main.lua`.

- **Windows:** Visual Studio or MinGW. Only system DLLs are linked (MFPlat, MFReadWrite, ole32).
- **macOS:** Add `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` for a universal dylib.
- **Linux:** Install the development dependencies:

  ```bash
  sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
  ```

  Players need `gstreamer1.0-plugins-good` and `gstreamer1.0-libav` (most desktops already have them).

Run the tests with:

```bash
love test
```

Run this command from the project folder.

## Status

| Platform | Built | Tested |
|---|---|---|
| Linux / GStreamer | Yes | 17/17 tests in LOVE 11.5 |
| Windows / Media Foundation | Yes (MinGW cross-compile) | 17/17 tests in Windows LOVE 11.5 under Wine. 17/17 tests in Windows LOVE 12.0. |


## Limitations / next steps

- **Audio isn't handled.** Extract it once using FFmpeg:

  ```bash
  ffmpeg -i in.mp4 -vn -c:a libvorbis out.ogg
  ```

  Play it with `love.audio`, and use `video:setTime(source:tell())` to keep the picture in sync.

- **Decoding happens on the main thread** during `update`/`setTime`. GStreamer already decodes ahead in its own threads. On Windows/macOS, a 1080p frame takes a few milliseconds. For 4K, move decoding into a `love.thread`.
- **Windows hardware decode:** Media Foundation is allowed to use hardware decoders, but full GPU decoding needs a Direct3D device manager, which this version doesn't set up.
- **Alpha:** No OS decoder handles transparent video well. For alpha, use the `.gv` player.
- **Video rotation metadata:** Phone video rotation is ignored.

## Credits

Thanks to **ffunatsu** and their `love2d_gv_video_player`. 
