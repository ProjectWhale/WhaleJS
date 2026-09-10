-- WhaleJS build (xmake)
-- src/        我们的 QuickJS 改造源码（基于 QuickJS-ng）
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

-- 引擎源码（WhaleJS 的 JIT/AOT 接口实现内嵌在 quickjs.c 里，见 src/whalejs.h）
local engine_sources = {
    SRC .. "/quickjs.c", SRC .. "/dtoa.c", SRC .. "/libregexp.c",
    SRC .. "/libunicode.c", SRC .. "/quickjs-libc.c",
}

-- Windows/MinGW 链接参数对齐 quickjs-ng 的 cmake MINGW 分支
local function link_whale()
    if is_win() then
        add_ldflags("-static", "-static-libgcc", "-Wl,--stack,8388608", { force = true })
        add_syslinks("pthread")
    else
        add_syslinks("m", "pthread", "dl")
    end
end

-- qjs CLI（对照与手工验证用）
target("qjs")
    set_kind("binary")
    add_files(engine_sources)
    add_files(SRC .. "/qjs.c", SRC .. "/gen/repl.c", SRC .. "/gen/standalone.c")
    add_includedirs(SRC)
    add_defines(qjs_defines)
    link_whale()

-- WhaleJS 单元测试：xmake run whale_test
target("whale_test")
    set_kind("binary")
    add_files(engine_sources)
    add_files("tests/whale/*.c")
    add_includedirs(SRC)
    add_defines(qjs_defines)
    link_whale()
