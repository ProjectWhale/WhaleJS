-- WhaleJS build (xmake)
-- src/        我们的 QuickJS 改造源码（基于 QuickJS-ng，待侵入式修改）
-- thirdparty/ 依赖源码（TinyCC 等）
-- temp/       临时数据（下载物、baseline 构建目录），不入 git
set_project("WhaleJS")
set_version("0.0.1")
set_languages("c11", "c++14")
add_rules("mode.debug", "mode.release")

local SRC = "src"

-- MinGW 会被 xmake 识别成独立平台 (mingw)，同样走 Windows 链接分支
local function is_win()
    return is_plat("windows") or is_plat("mingw")
end

local qjs_defines = { "_GNU_SOURCE" }
if is_win() then
    table.join2(qjs_defines, { "WIN32_LEAN_AND_MEAN", "_WIN32_WINNT=0x0601" })
end

-- 原版快速验证目标（未接入 JIT 的 QuickJS-ng CLI，对照 cmake 成功链接参数）
target("qjs")
    set_kind("binary")
    add_files(SRC .. "/quickjs.c", SRC .. "/dtoa.c", SRC .. "/libregexp.c",
              SRC .. "/libunicode.c", SRC .. "/quickjs-libc.c")
    add_files(SRC .. "/qjs.c", SRC .. "/gen/repl.c", SRC .. "/gen/standalone.c")
    add_includedirs(SRC)
    add_defines(qjs_defines)
    if is_win() then
        add_ldflags("-static", "-static-libgcc", "-Wl,--stack,8388608", { force = true })
        add_syslinks("pthread")
    else
        add_syslinks("m", "pthread", "dl")
    end
