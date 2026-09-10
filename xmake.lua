-- WhaleJS build (xmake)
-- src/                    我们的 QuickJS 改造源码（基于 QuickJS-ng）
-- thirdparty/tinycc      TinyCC 源码（JIT 代码生成后端；ONE_SOURCE 单文件编译）
-- thirdparty/tcc-runtime TCC 运行时支持（config.h / include / libtcc1-64.a / *.def）
-- temp/                   临时数据（下载物、baseline 构建目录），不入 git
set_project("WhaleJS")
set_version("0.0.1")
set_languages("c11", "c++14")
add_rules("mode.debug", "mode.release")

local SRC = "src"
local TCC = "thirdparty/tinycc"
local TCC_RT = "thirdparty/tcc-runtime"

-- MinGW 会被 xmake 识别成独立平台 (mingw)，同样走 Windows 链接分支
local function is_win()
    return is_plat("windows") or is_plat("mingw")
end

local qjs_defines = { "_GNU_SOURCE" }

-- The JIT backend runs at runtime and needs to find quickjs.h and the TCC
-- runtime next to the sources, whatever the working directory is, so bake the
-- absolute paths in at configure time.
-- NB: backslashes would be eaten as escapes inside the C string literal
local PROJ = string.gsub(os.projectdir(), "\\", "/")
table.join2(qjs_defines, {
    'WHALE_JIT_SRC_DIR="' .. PROJ .. '/src"',
    'WHALE_JIT_TCC_DIR="' .. PROJ .. '/thirdparty/tcc-runtime"',
})
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

-- TinyCC：JIT 运行期把生成的 C 源码编成机器码。
-- tcc.h 里 ONE_SOURCE 默认 1，所以整个 libtcc 只有 libtcc.c 一个翻译单元。
-- config.h 放在 tcc-runtime/（TinyCC 上游 .gitignore 会吃掉 tinycc/config*.h）。
target("libtcc")
    set_kind("static")
    add_files(TCC .. "/libtcc.c")
    add_includedirs(TCC, TCC_RT)
    add_defines("TCC_TARGET_PE", "TCC_TARGET_X86_64")

-- qjs CLI（对照与手工验证用）
target("qjs")
    set_kind("binary")
    set_rundir("$(projectdir)")
    add_deps("libtcc")
    add_files(engine_sources)
    add_files(SRC .. "/qjs.c", SRC .. "/gen/repl.c", SRC .. "/gen/standalone.c")
    add_includedirs(SRC, TCC)
    add_defines(qjs_defines)
    link_whale()

-- WhaleJS 单元测试：xmake run whale_test
target("whale_test")
    set_kind("binary")
    set_rundir("$(projectdir)")
    add_deps("libtcc")
    add_files(engine_sources)
    add_files("tests/whale/*.c")
    add_includedirs(SRC, TCC)
    add_defines(qjs_defines)
    link_whale()
