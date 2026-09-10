-- WhaleJS build (xmake)
-- src/                    我们的 QuickJS 改造源码（基于 QuickJS-ng）
-- thirdparty/tinycc      TinyCC 源码（JIT 代码生成后端；ONE_SOURCE 单文件编译）
-- thirdparty/tcc-runtime TCC 运行时支持（config.h / include / libtcc1-64.a / *.def）
-- temp/                   临时数据（下载物、baseline 构建目录），不入 git
set_project("WhaleJS")
set_version("0.0.1")
set_languages("c11", "c++14")
add_rules("mode.debug", "mode.release")

-- libtcc: compiled from thirdparty/tinycc by default (we intend to patch it),
-- or linked from the prebuilt DLL+import lib with --libtcc_prebuilt=y.
option("libtcc_prebuilt")
    set_default(false)
    set_showmenu(true)
    set_description("Link the prebuilt libtcc (DLL + import library) instead of compiling TinyCC from source")
option_end()

local libtcc_prebuilt = has_config("libtcc_prebuilt")

local SRC = "src"
local TCC = "src/tcc"
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
    SRC .. "/quickjs.c", SRC .. "/whale_jit.c", SRC .. "/dtoa.c", SRC .. "/libregexp.c",
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
-- TinyCC sources live in src/tcc and are built once as a static library that
-- qjs/whale_test link.
-- NB: do NOT include libtcc.c into quickjs.c - both files expand their token
-- tables through a DEF() macro, so one translation unit gives duplicate
-- enumerators (TOK_VOID, TOK_IF, ...). To use TinyCC's internals from the JIT
-- codegen, add thin exported wrappers on the TinyCC side (the same pattern the
-- libtcc API itself uses) and declare them here.
local function link_libtcc()
    add_includedirs(TCC, TCC_RT)
    if libtcc_prebuilt then
        add_defines("WHALE_LIBTCC_PREBUILT")
        add_linkdirs(TCC_RT .. "/lib")
        add_links("libtcc")
        after_build(function (target)
            os.cp(TCC_RT .. "/lib/libtcc.dll", target:targetdir())
        end)
    else
        add_deps("libtcc")
    end
end

if not libtcc_prebuilt then
    target("libtcc")
        set_kind("static")
        add_files(TCC .. "/libtcc.c")
        add_includedirs(TCC, TCC_RT)
        add_defines("TCC_TARGET_PE", "TCC_TARGET_X86_64")
end

-- qjs CLI（对照与手工验证用）
target("qjs")
    set_kind("binary")
    set_rundir("$(projectdir)")
    link_libtcc()
    add_files(engine_sources)
    add_files(SRC .. "/qjs.c", SRC .. "/gen/repl.c", SRC .. "/gen/standalone.c")
    add_includedirs(SRC, TCC)
    add_defines(qjs_defines)
    link_whale()

-- WhaleJS 单元测试：xmake run whale_test
target("whale_test")
    set_kind("binary")
    set_rundir("$(projectdir)")
    link_libtcc()
    add_files(engine_sources)
    add_files("tests/whale/*.c")
    add_includedirs(SRC, TCC)
    add_defines(qjs_defines)
    link_whale()
