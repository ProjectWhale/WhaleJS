-- WhaleJS build (xmake)
-- src/                    我们的 QuickJS 改造源码（基于 QuickJS-ng）
-- src/tcc                  TinyCC 源码（JIT 代码生成后端；whale_tcc_iface.c 里 #include libtcc.c，单 TU）
-- thirdparty/tcc-runtime  TCC 运行时支持（config.h / include / libtcc1-64.a / *.def）
-- temp/                    临时数据（下载物、baseline 构建目录），不入 git
--
-- 产物（本工程是**库**，不是专用解释器）：
--   libwhalejs.a        静态库（引擎 + libc + TCC JIT 后端）
--   whalejs.dll/.so     动态库（Windows 另有导入库 whalejs.dll.a）
--   whalejs(.exe)       CLI，兼作手工验证与 perftest 的引擎命令
--   whale_test(.exe)    单元测试
--   whale_demo(.exe)    嵌入示例（只依赖公开头文件 + 库，验证分发形态可用）
--   xmake whale_pack    按开源惯例把 include/lib/README 打成一个归档
set_project("WhaleJS")
-- WhaleJS 自己的版本，与 QuickJS-ng 解耦（上游升级不跟它的号）。
-- MAJOR=接口/ABI 不兼容，MINOR=新优化/新覆盖，PATCH=修 bug。见 CHANGELOG.md。
set_version("0.2.0")
set_languages("c11", "c++14")
add_rules("mode.debug", "mode.release")

-- 诊断开关（WHALE_JIT_DUMP / WHALE_STR_STAT / ... 这类环境变量）只进调试构建。
-- 这是引擎本身的开发工具，不是跑在引擎上的程序的配置；发布二进制里没有这些读取。
option("whale_debug")
    set_default(false)
    set_showmenu(true)
    set_description("Compile in the WHALE_* diagnostic switches (JIT dumps, helper counters)")
option_end()

-- 打包：xmake whale_pack
-- 把 include/（公开头文件）与 lib/（库文件）按开源惯例打成一个 zip。
-- 刻意不放的东西：
--   * README —— 用户自备（见 temp/README.md 草稿），发布时再补；
--   * tcc-runtime —— 已硬编码进库（whale_libtcc1_a.inc，首次运行解到 %TEMP%），
--     包里没有任何 tcc 相关文件；
--   * demo/CLI exe —— 归档只有库，没有可执行文件。
-- 打包逻辑在 tools/whale_pack.lua（用 xmake l 跑，那里 io 完整可用，zip 纯 Lua
-- 写，不依赖外部 tar/zip）；task 只负责保证库已构建并传配置。
task("whale_pack")
    set_menu {
        usage  = "xmake whale_pack",
        description = "Build a release archive (include/lib only) for WhaleJS",
    }
    on_run(function ()
        import("core.project.config")
        import("core.project.task")

        os.execv("xmake", { "build", "-y", "whalejs_static", "whalejs_shared" })
        os.setenv("WHALE_PACK_PROJECT", os.projectdir())
        os.setenv("WHALE_PACK_PLAT", config.get("plat") or os.host())
        os.setenv("WHALE_PACK_ARCH", config.get("arch") or os.arch())
        os.setenv("WHALE_PACK_MODE", config.get("mode") or "release")
        os.setenv("WHALE_PACK_VER", "0.2.0")
        os.execv("xmake", { "l", path.join(os.projectdir(), "tools", "whale_pack.lua") })
    end)
task_end()

local SRC = "src"
local TCC = "src/tcc"
local TCC_RT = "thirdparty/tcc-runtime"

-- MinGW 会被 xmake 识别成独立平台 (mingw)，同样走 Windows 链接分支
local function is_win()
    return is_plat("windows") or is_plat("mingw")
end

local qjs_defines = { "_GNU_SOURCE" }

-- The JIT backend runs at runtime and needs to find the TCC runtime next to the
-- sources, whatever the working directory is, so bake the absolute paths in at
-- configure time. These are only the last-resort fallback: a released package
-- locates its own tcc-runtime relative to the library (JS_WhaleSetJitRuntimeDir
-- / module-relative probe in whale_jit.c).
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
-- x86-64 直发机器码后端（route B）：只在 x86_64 编译，与 TCC 后端共存
-- （wrapper 仍走 TCC，只有 numeric core 走这里）。
if is_arch("x86_64") then
    table.insert(engine_sources, SRC .. "/whale_emit_x64.c")
end

-- Windows/MinGW 链接参数对齐 quickjs-ng 的 cmake MINGW 分支。
-- export_all 只给可执行文件：共享库的导出面由 quickjs.h 的 BUILDING_QJS_SHARED
-- （JS_EXTERN = dllexport）决定，不需要也不应该整表导出。
local function link_whale(export_all)
    if is_win() then
        add_ldflags("-static", "-static-libgcc", "-Wl,--stack,8388608", { force = true })
        if export_all then
            add_ldflags("-Wl,--export-all-symbols", { force = true })
        end
        add_syslinks("pthread")
    else
        add_syslinks("m", "pthread", "dl")
    end
end

-- TinyCC：JIT 运行期把生成的代码编成机器码。
-- tcc.h 里 ONE_SOURCE 默认 1，所以整个 libtcc 只有 whale_tcc_iface.c（它
-- #include "libtcc.c"）一个翻译单元，TCC 源码与它编译在同一个 TU 里。
-- config.h 放在 tcc-runtime/（TinyCC 上游 .gitignore 会吃掉 tinycc/config*.h）。
-- NB: do NOT include libtcc.c into quickjs.c - both files expand their token
-- tables through a DEF() macro, so one translation unit gives duplicate
-- enumerators (TOK_VOID, TOK_IF, ...). To use TinyCC's internals from the JIT
-- codegen, add thin exported wrappers on the TinyCC side (the same pattern the
-- libtcc API itself uses) and declare them here.
local function engine_common()
    add_files(engine_sources)
    add_files(TCC .. "/whale_tcc_iface.c")
    -- TinyCC's runtime helpers (__divdi3/__fixunsdfdi/...), compiled into the
    -- library so the JIT needs no libtcc1-64.a at run time: they are registered
    -- as host symbols in whale_jit.c. Their own TU - they are plain C with no
    -- TCC-specific macros.
    add_files(TCC .. "/lib/libtcc1.c")
    add_includedirs(SRC, TCC, TCC_RT)
    add_defines(qjs_defines)
    -- TCC_TARGET_* 只对 whale_tcc_iface.c 有意义；放在 target 级是为了让
    -- 单文件编译参数保持简单（引擎源码不引用这两个宏）。
    add_defines("TCC_TARGET_PE", "TCC_TARGET_X86_64")
    if has_config("whale_debug") then
        add_defines("WHALE_DEBUG")
    end
end

-- 静态库 libwhalejs.a。引擎源码直接进这个 target（不用 object 中转：
-- 实测"只有 object 依赖、自己没有源文件"的 static target 不会出现在使用者的
-- 链接行上）。公开头文件目录以 public 形式带出去，依赖方自动获得 -I。
target("whalejs_static")
    set_kind("static")
    set_basename("whalejs")
    engine_common()
    add_includedirs(SRC, { public = true })
    link_whale(false)

-- 动态库 whalejs.dll / libwhalejs.so（Windows 另出导入库 whalejs.dll.a）。
-- BUILDING_QJS_SHARED 让 quickjs.h 的 JS_EXTERN 变成 __declspec(dllexport)
-- （Windows）/ visibility("default")（ELF），即导出面由头文件声明决定。
-- Windows 上 DLL 没有 lib 前缀（惯例是 whalejs.dll），MinGW 工具链默认会加，
-- 所以显式清掉；ELF 平台保留 libwhalejs.so。
target("whalejs_shared")
    set_kind("shared")
    set_basename("whalejs")
    if is_win() then
        set_prefixname("")
    end
    engine_common()
    add_defines("BUILDING_QJS_SHARED")
    if not is_win() then
        add_cxflags("-fPIC", { force = true })
    end
    add_includedirs(SRC, { public = true })
    -- 使用者需要 USING_QJS_SHARED 才能把 JS_EXTERN 认成 dllimport（Windows）。
    add_defines("USING_QJS_SHARED", { interface = true })
    link_whale(false)

-- whalejs CLI（手工验证与 perftest 的引擎命令名；产物名 whalejs.exe）
target("whalejs_cli")
    set_kind("binary")
    set_basename("whalejs")
    set_rundir("$(projectdir)")
    add_deps("whalejs_static")
    add_files(SRC .. "/qjs.c", SRC .. "/gen/repl.c", SRC .. "/gen/standalone.c")
    add_includedirs(SRC, TCC)
    add_defines(qjs_defines)
    link_whale(true)

-- WhaleJS 单元测试：xmake run whale_test
target("whale_test")
    set_kind("binary")
    set_rundir("$(projectdir)")
    add_deps("whalejs_static")
    add_files("tests/whale/*.c")
    add_includedirs(SRC, TCC)
    add_defines(qjs_defines)
    link_whale(true)

-- 嵌入示例：只 include 公开头文件、只链 libwhalejs.a，验证分发形态本身可用。
-- 不参与打包（归档里没有 demo exe）。
target("whale_demo")
    set_kind("binary")
    set_rundir("$(projectdir)")
    add_deps("whalejs_static")
    add_files("examples/embed.c")
    add_includedirs(SRC)
    add_defines(qjs_defines)
    link_whale(false)
