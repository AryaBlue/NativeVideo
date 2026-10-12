-- Automated test: love test/   (run from the project root, after building)
-- counter.mp4 is a 2s, 30fps video with the frame number drawn as six
-- black/white bit blocks, so we can read back exactly which frame is on screen.
package.path = "../?.lua;" .. love.filesystem.getSourceBaseDirectory() .. "/?.lua;" .. package.path
local NativeVideo = require("NativeVideo")

local media = love.filesystem.getSource() .. "/media/"
local failures, checks = 0, 0

local function frame_on_screen(v)
    -- Six 64px-wide blocks, block b is white when bit b of the frame number is set.
    local n = 0
    for b = 0, 5 do
        if v.front:getPixel(b * 64 + 32, v.height / 2) > 0.5 then n = n + 2 ^ b end
    end
    return n
end

local function check(name, got, want)
    checks = checks + 1
    if got == want then
        print(("  ok   %-40s %s"):format(name, tostring(got)))
    else
        failures = failures + 1
        print(("  FAIL %-40s got %s, want %s"):format(name, tostring(got), tostring(want)))
    end
end

function love.load()
    print("backend: " .. NativeVideo.getBackend())

    print("counter.mp4 (h264 + aac audio track)")
    local v = NativeVideo.new(media .. "counter.mp4")
    check("size", v.width .. "x" .. v.height, "384x96")
    check("fps", v.fps, 30)
    check("duration ~2s", math.abs(v:getDuration() - 2) < 0.05, true)
    check("first frame shown on open", frame_on_screen(v), 0)

    v:play()
    for _ = 1, 10 do v:update(1 / 30) end          -- t = 0.333
    check("after 10 frame steps", frame_on_screen(v), 10)

    v:update(0.5)                                     -- big dt: frames skipped
    check("after lag spike (t=0.833)", frame_on_screen(v), 25)

    v:seek(1.0)
    check("seek(1.0)", frame_on_screen(v), 30)
    v:seek(1.51)
    check("seek(1.51) mid-frame", frame_on_screen(v), 45)
    v:seek(0.2)
    check("seek backwards to 0.2", frame_on_screen(v), 6)

    v:setTime(0.5)
    check("setTime forward small (0.5)", frame_on_screen(v), 15)
    v:setTime(0.1)
    check("setTime backwards (0.1)", frame_on_screen(v), 3)
    v:setTime(1.9)
    check("setTime big jump (1.9)", frame_on_screen(v), 57)

    v:setLoop(false)
    for _ = 1, 30 do v:update(1 / 30) end
    check("stops at end without loop", v:isPlaying(), false)
    check("last frame stays visible", frame_on_screen(v), 59)

    v:seek(1.8); v:setLoop(true); v:play()
    for _ = 1, 9 do v:update(1 / 30) end             -- t = 2.1 -> wraps to 0.1
    check("loop wraps to start", frame_on_screen(v), 3)
    v:release()

    print("vp9.webm")
    local w = NativeVideo.new(media .. "vp9.webm")
    check("vp9 size", w.width .. "x" .. w.height, "1280x720")
    w:release()

    -- Optional speed test: drop any 1080p video in as media/big1080p60.mp4
    local big = io.open(media .. "big1080p60.mp4", "rb")
    if big then big:close() end
    print("big1080p60.mp4 (decode speed)" .. (big and "" or " - skipped, file not present"))
    local b = big and NativeVideo.new(media .. "big1080p60.mp4")
    if b then
    b:play()
    local t0 = love.timer.getTime()
    local n = 0
    while b:isPlaying() and n < 600 do b:update(1 / 60); n = n + 1 end
    local elapsed = love.timer.getTime() - t0
    print(("  %d frames in %.2fs = %.0f fps (decode + copy + GPU upload)"):format(n, elapsed, n / elapsed))
    b:release()
    end

    local ok, err = pcall(NativeVideo.new, media .. "does_not_exist.mp4")
    check("missing file errors cleanly", ok, false)

    print(("%d/%d checks passed"):format(checks - failures, checks))
    love.event.quit(failures == 0 and 0 or 1)
end
