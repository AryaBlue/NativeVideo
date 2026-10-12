-- Demo: drag a video file onto the window, or run  love . path/to/video.mp4
--
--   Space      play / pause
--   Left/Right seek -5s / +5s
--   Home       back to start
--   L          toggle loop

local NativeVideo = require("NativeVideo")

local video
local message = "Drop a video file (.mp4, .mov, .webm...) onto this window"

local function open(path)
    if video then video:release(); video = nil end
    local ok, result = pcall(NativeVideo.new, path)
    if not ok then
        message = tostring(result)
        print(message)
        return
    end
    video = result
    video:setLoop(true)
    video:play()
    print(string.format("Opened %s: %dx%d, %.2f fps, %.2fs, backend=%s",
        path, video.width, video.height, video.fps, video:getDuration(),
        NativeVideo.getBackend()))
end

function love.load(args)
    love.window.setTitle("NativeVideo demo")
    if args and args[1] then open(args[1]) end
end

function love.filedropped(file)
    open(file:getFilename())   -- dropped files give a real OS path
end

function love.keypressed(key)
    if key == "escape" then love.event.quit() end
    if not video then return end
    if key == "space" then
        if video:isPlaying() then video:pause() else video:play() end
    elseif key == "left" then
        video:seek(video:tell() - 5)
    elseif key == "right" then
        video:seek(video:tell() + 5)
    elseif key == "home" then
        video:seek(0)
    elseif key == "l" then
        video:setLoop(not video:isLooping())
    end
end

function love.update(dt)
    if video then video:update(dt) end
end

function love.draw()
    local ww, wh = love.graphics.getDimensions()
    if not video then
        love.graphics.printf(message, 20, wh / 2 - 20, ww - 40, "center")
        return
    end

    -- Fit the video inside the window, keeping its aspect ratio.
    local vw, vh = video:getDimensions()
    local s = math.min(ww / vw, wh / vh)
    video:draw((ww - vw * s) / 2, (wh - vh * s) / 2, 0, s, s)

    love.graphics.setColor(0, 0, 0, 0.6)
    love.graphics.rectangle("fill", 0, 0, ww, 24)
    love.graphics.setColor(1, 1, 1)
    love.graphics.print(string.format("%s  %.2f / %.2fs   %s   loop:%s   %d fps",
        NativeVideo.getBackend(), video:tell(), video:getDuration(),
        video:isPlaying() and "playing" or "paused",
        video:isLooping() and "on" or "off", love.timer.getFPS()), 6, 5)
end
