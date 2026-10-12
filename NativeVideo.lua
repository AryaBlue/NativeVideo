local ffi = require("ffi")

ffi.cdef[[
typedef struct nv_video nv_video;
typedef struct nv_info { int32_t width, height; double fps, duration; } nv_info;
nv_video*   nv_open(const char* path_utf8);
void        nv_get_info(nv_video* v, nv_info* out);
int         nv_read_frame(nv_video* v, uint8_t* rgba_out, double* pts_out);
int         nv_seek(nv_video* v, double seconds);
void        nv_close(nv_video* v);
const char* nv_last_error(void);
const char* nv_backend(void);
]]

local NativeVideo = {}
NativeVideo.__index = NativeVideo

local SEEK_THRESHOLD = 1.0
local lib

local function load_library()
    if lib then return lib end
    local os_name = love.system.getOS()
    local file = os_name == "Windows" and "nativevideo.dll"
              or os_name == "OS X"    and "libnativevideo.dylib"
              or "libnativevideo.so"

    --for packing
    local candidates = {}
    if not love.filesystem.isFused() then
        table.insert(candidates, love.filesystem.getSource() .. "/" .. file)
    end
    table.insert(candidates, love.filesystem.getSourceBaseDirectory() .. "/" .. file)
    table.insert(candidates, "nativevideo")

    local errors = {}
    for _, path in ipairs(candidates) do
        local ok, result = pcall(ffi.load, path)
        if ok then lib = result; return lib end
        table.insert(errors, result)
    end
    error("NativeVideo: could not load " .. file .. ":\n  " .. table.concat(errors, "\n  "), 3)
end


local function file_exists(path)
    local f = io.open(path, "rb")
    if f then f:close() return true end
    return false
end

local function is_absolute(path)
    return path:sub(1, 1) == "/" or path:match("^%a:[/\\]") ~= nil
end

local function resolve_path(path)
    if is_absolute(path) and file_exists(path) then return path end

    if love.filesystem.getInfo(path, "file") then
        local dir = love.filesystem.getRealDirectory(path)
        if dir then
            local real = dir .. "/" .. path
            if file_exists(real) then return real end
        end

        local cache = "nativevideo_cache/" .. path:gsub("[/\\]", "_")
        if not love.filesystem.getInfo(cache, "file") then
            love.filesystem.createDirectory("nativevideo_cache")
            local data = assert(love.filesystem.read("data", path))
            assert(love.filesystem.write(cache, data))
            data:release()
        end
        return love.filesystem.getSaveDirectory() .. "/" .. cache
    end

    error("NativeVideo: file not found: " .. tostring(path), 3)
end

function NativeVideo.new(path)
    local L = load_library()
    local real_path = resolve_path(path)

    local handle = L.nv_open(real_path)
    if handle == nil then
        error("NativeVideo: " .. ffi.string(L.nv_last_error()), 2)
    end
    handle = ffi.gc(handle, L.nv_close)   -- closed automatically

    local info = ffi.new("nv_info")
    L.nv_get_info(handle, info)

    local self = setmetatable({}, NativeVideo)
    self.handle   = handle
    self.path     = real_path
    self.width    = info.width
    self.height   = info.height
    self.fps      = info.fps > 0 and info.fps or 30
    self.duration = info.duration

    -- Two CPU-side frames: front is what's on screen back holds the next
    self.front = love.image.newImageData(self.width, self.height, "rgba8")
    self.back  = love.image.newImageData(self.width, self.height, "rgba8")
    self.image = love.graphics.newImage(self.front)
    self.pts   = ffi.new("double[1]")

    self.time     = 0       -- current playback position (seconds)
    self.backPts  = nil
    self.eof      = false
    self.playing  = false
    self.loop     = false
    self.speed    = 1
    self.frameIndex = 0     -- frames shown so far (for stats)

    self:_decodeIntoBack()
    self:_presentBack()
    return self
end

---Not gonna pretend to understand this thanks bestie <3
-- Decode the next frame into `back`. Returns true if a frame was read.
function NativeVideo:_decodeIntoBack()
    local ptr = ffi.cast("uint8_t*", self.back:getFFIPointer())
    local r = lib.nv_read_frame(self.handle, ptr, self.pts)
    if r == 1 then

        if not self.ptsOffset then self.ptsOffset = self.pts[0] end
        self.backPts = self.pts[0] - self.ptsOffset
        return true
    elseif r == 0 then
        self.backPts, self.eof = nil, true
        if self.duration <= 0 then self.duration = self.time end
        return false
    else
        error("NativeVideo: " .. ffi.string(lib.nv_last_error()), 3)
    end
end

-- Swap the waiting frame onto the screen and start decoding the next one.
function NativeVideo:_presentBack()
    self.front, self.back = self.back, self.front
    self.dirty = true
    self.frameIndex = self.frameIndex + 1
    self:_decodeIntoBack()
end

-- Show every frame whose time has come
function NativeVideo:_advanceTo(t)
    -- exact frame time, which would otherwise show the frame one tick late.
    t = t + 1e-6
    while self.backPts and self.backPts <= t do
        self:_presentBack()
    end
    if self.dirty then
        self.image:replacePixels(self.front)
        self.dirty = false
    end
end

function NativeVideo:update(dt)
    if not self.playing then return end
    self.time = self.time + dt * self.speed
    self:_advanceTo(self.time)

    if self.eof and self.duration > 0 and self.time >= self.duration then
        if self.loop then
            self:seek(self.time % self.duration)
        else
            self.playing = false
        end
    end
end

function NativeVideo:seek(t)
    t = math.max(0, t)
    if self.duration > 0 then t = math.min(t, self.duration) end
    local target = t + 1e-6
    local offset = self.ptsOffset or 0

    -- starts at the target, Media Foundation may land on the keyframe before
    local margin = 0
    for _ = 1, 5 do
        if lib.nv_seek(self.handle, math.max(0, t + offset - margin)) ~= 1 then
            error("NativeVideo: " .. ffi.string(lib.nv_last_error()), 2)
        end
        self.eof = false
        local got = self:_decodeIntoBack()
        if not got or self.backPts <= target or t + offset - margin <= 0 then break end
        margin = margin + (self.backPts - t) + 1 / self.fps
    end
    self.time = t

    -- Show the last frame that starts at or before t.
    if self.backPts then
        self:_presentBack()
        while self.backPts and self.backPts <= target do
            self:_presentBack()
        end
    end
    self.image:replacePixels(self.front)
    self.dirty = false
end

--- Follow an external clock, video:setTime(music:tell()).
function NativeVideo:setTime(t)
    if t < self.time or t - self.time > SEEK_THRESHOLD then
        self:seek(t)
    else
        self.time = t
        self:_advanceTo(t)
    end
end

function NativeVideo:play()  self.playing = true end
function NativeVideo:pause() self.playing = false end
function NativeVideo:stop()  self.playing = false; self:seek(0) end
function NativeVideo:isPlaying() return self.playing end

function NativeVideo:setLoop(loop) self.loop = not not loop end
function NativeVideo:isLooping() return self.loop end
function NativeVideo:setSpeed(s) self.speed = s end

function NativeVideo:tell() return self.time end
function NativeVideo:getDuration() return self.duration end
function NativeVideo:getDimensions() return self.width, self.height end
function NativeVideo:getWidth() return self.width end
function NativeVideo:getHeight() return self.height end
function NativeVideo:getFPS() return self.fps end
function NativeVideo:getImage() return self.image end

function NativeVideo:draw(...)
    love.graphics.draw(self.image, ...)
end

function NativeVideo:release()
    if self.handle then
        lib.nv_close(ffi.gc(self.handle, nil))
        self.handle = nil
    end
    if self.image then self.image:release(); self.image = nil end
    if self.front then self.front:release(); self.front = nil end
    if self.back then self.back:release(); self.back = nil end
end

function NativeVideo.getBackend()
    return ffi.string(load_library().nv_backend())
end

return NativeVideo
