-- WhaleJS release packer.
-- Runs as: xmake l tools/whale_pack.lua
-- Stages include/ + lib/ (no README, no tcc-runtime - generated code only
-- references host symbols, so no TCC runtime file is needed at run time) and
-- writes a .zip with pure Lua (store method),
-- no external tar/zip tool.
--
-- Reads the configured platform from xmake via an env var set by the task.
local M = {}

local projectdir = os.getenv("WHALE_PACK_PROJECT") or "."
local plat = os.getenv("WHALE_PACK_PLAT") or "windows"
local arch = os.getenv("WHALE_PACK_ARCH") or "x64"
local mode = os.getenv("WHALE_PACK_MODE") or "release"
local ver = os.getenv("WHALE_PACK_VER") or "0.2.0"

local base = "whalejs-" .. ver .. "-" .. plat .. "-" .. arch
local outputdir = projectdir .. "/build/package"
local barch = (arch == "x64") and "x86_64" or arch
local tdir = projectdir .. "/build/" .. plat .. "/" .. barch .. "/" .. mode
if not os.isdir(tdir) then
    local alt = projectdir .. "/build/mingw/" .. barch .. "/" .. mode
    if os.isdir(alt) then
        tdir = alt
    end
end

local stage = outputdir .. "/" .. base
local function rm(path)
    os.rm(path)
end
if os.isdir(stage) then rm(stage) end
os.mkdir(stage .. "/include")
os.mkdir(stage .. "/lib")

-- public headers
local inc = projectdir .. "/src"
os.cp(inc .. "/quickjs.h", stage .. "/include")
os.cp(inc .. "/quickjs-libc.h", stage .. "/include")
os.cp(inc .. "/whalejs.h", stage .. "/include")
os.cp(inc .. "/LICENSE", stage .. "/include")

-- libraries
os.cp(tdir .. "/libwhalejs.a", stage .. "/lib")
-- The WebAssembly extension is a separate, optional static library: link it
-- only if the program wants wasm. Its public header (whalejs.h) is staged
-- above already, so no extra include is needed.
if os.isfile(tdir .. "/libWhaleJS-wasm.a") then
    os.cp(tdir .. "/libWhaleJS-wasm.a", stage .. "/lib")
end
if plat == "windows" or plat == "mingw" then
    os.cp(tdir .. "/whalejs.dll", stage .. "/lib")
    os.cp(tdir .. "/whalejs.dll.a", stage .. "/lib")
elseif plat == "macos" then
    os.cp(tdir .. "/libwhalejs.dylib", stage .. "/lib")
else
    os.cp(tdir .. "/libwhalejs.so", stage .. "/lib")
end
print("pack: staged " .. stage)

-- collect files under the stage dir
local files = {}
for _, f in ipairs(os.files(stage .. "/**")) do
    table.insert(files, f)
end
table.sort(files)

-- minimal ZIP writer (store)
local function le16(n) return string.pack("<I2", n) end
local function le32(n) return string.pack("<I4", n) end
local function crc32(data)
    local crc = 0xffffffff
    for i = 1, #data do
        local byte = string.byte(data, i)
        for _ = 1, 8 do
            local b = byte % 2
            byte = math.floor(byte / 2)
            if (crc % 2) ~= b then
                crc = math.floor(crc / 2)
                crc = crc ~ 0xedb88320
            else
                crc = math.floor(crc / 2)
            end
        end
    end
    return crc ~ 0xffffffff
end

local out = io.open(outputdir .. "/" .. base .. ".zip", "wb")
if not out then
    error("whale_pack: cannot open output zip")
end
local central = {}
local offset = 0

local function add_entry(name, data)
    local crc = crc32(data)
    local csize = #data
    local lh = "PK\3\4"
        .. le16(20) .. le16(0) .. le16(0)
        .. le16(0) .. le16(0)
        .. le32(crc)
        .. le32(csize) .. le32(csize)
        .. le16(#name) .. le16(0)
        .. name .. data
    table.insert(central, { name = name, crc = crc, csize = csize, offset = offset })
    out:write(lh)
    offset = offset + #lh
end

-- normalize a path to forward slashes (Lua patterns hate backslashes)
local function norm(p)
    return string.gsub(p, "\\", "/")
end
local stage_n = norm(stage)

local function rel(f)
    local p = norm(f)
    -- plain prefix check, not a pattern: the version string contains '.', and
    -- '^' + stage as a Lua pattern would treat it as a wildcard
    if string.sub(p, 1, #stage_n) == stage_n then
        return string.sub(p, #stage_n + 2)
    end
    return p
end

add_entry(base .. "/", "")
for _, f in ipairs(files) do
    local fh = io.open(f, "rb")
    if not fh then
        error("whale_pack: cannot read " .. f)
    end
    local data = fh:read("*a")
    fh:close()
    add_entry(base .. "/" .. rel(f), data)
end

local cd_start = offset
local cd_parts = {}
for _, c in ipairs(central) do
    table.insert(cd_parts, "PK\1\2"
        .. le16(20) .. le16(20)
        .. le16(0) .. le16(0)
        .. le16(0) .. le16(0)
        .. le32(c.crc)
        .. le32(c.csize) .. le32(c.csize)
        .. le16(#c.name) .. le16(0) .. le16(0) .. le16(0) .. le16(0)
        .. le32(0)             -- external attributes
        .. le32(c.offset)
        .. c.name)
end
local cd_body = table.concat(cd_parts)
out:write(cd_body)
out:write("PK\5\6"
    .. le16(0) .. le16(0)
    .. le16(#central) .. le16(#central)
    .. le32(#cd_body)
    .. le32(cd_start)
    .. le16(0))
out:close()
print("whale pack: " .. outputdir .. "/" .. base .. ".zip")