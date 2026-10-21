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
set_languages("c11")
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
--   * tcc-runtime —— 运行库已被宿主符号取代（生成的代码只引用宿主符号，
--     CRT 不链接），包里没有任何 tcc 相关文件，运行期也不搜索任何目录；
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

-- TCC 的目标宏必须跟着构建目标走，不能写死。它同时决定两件事：
--   * TCC 生成哪个架构的代码（x86_64-gen.c / arm64-gen.c / ...）；
--   * TCC_IS_NATIVE 是否成立，即"宿主能不能直接执行生成的代码"。
-- 写死 x86_64 + PE 时交叉编译到 arm64 会同时炸两处：
--   * libtcc1.c 按宿主 aarch64 编译，却走进 32 位 x86 的除法辅助代码
--     （#error unsupported CPU type / udiv_qrnnd 未声明）；
--   * TCCState 里没有 runtime_main（TCC_IS_NATIVE 不成立），iface 编译失败。
-- tcc.h 自己也能从编译器预定义宏推断，但显式给出更可靠（MSVC/clang-cl 的
-- 预定义宏与 GCC 不同），而且构建系统本来就知道目标是什么。
--
-- 返回 nil 表示"这个平台/架构组合 TCC 后端不支持"。调用方据此**关闭 JIT**
-- （不编译 TCC、不定义 WHALE_JIT_AVAILABLE），引擎照常构建、走解释器：
-- 兼容性优先，绝不让一个 TCC 不支持的架构把整个库编译失败。
local function tcc_target_defines()
    if is_win() then
        if is_arch("x86_64") then
            return { "TCC_TARGET_PE", "TCC_TARGET_X86_64" }
        elseif is_arch("i386", "x86") then
            return { "TCC_TARGET_PE", "TCC_TARGET_I386" }
        elseif is_arch("arm64", "arm64-v8a", "aarch64") then
            -- Windows on ARM64: PE backend ported in tccpe.c/arm64-gen.c
            return { "TCC_TARGET_PE", "TCC_TARGET_ARM64" }
        end
    else
        if is_arch("x86_64") then
            return { "TCC_TARGET_X86_64" }
        elseif is_arch("arm64", "arm64-v8a", "aarch64") then
            return { "TCC_TARGET_ARM64" }
        elseif is_arch("arm", "armeabi-v7a") then
            return { "TCC_TARGET_ARM", "TCC_ARM_EABI", "TCC_ARM_HARDFLOAT" }
        end
    end
    return nil
end

-- Does the JIT exist on this target? Only the TCC backends above can produce
-- machine code for the host, so this is exactly "TCC has a target for it".
local tcc_defs = tcc_target_defines()
local jit_enabled = tcc_defs ~= nil

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
    if jit_enabled then
        add_files(TCC .. "/whale_tcc_iface.c")
        -- TinyCC's runtime helpers (__divdi3/__fixunsdfdi/...), compiled into
        -- the library so the JIT needs no libtcc1-64.a at run time: they are
        -- registered as host symbols in whale_jit.c. Their own TU - they are
        -- plain C with no TCC-specific macros.
        add_files(TCC .. "/lib/libtcc1.c")
        -- TCC_TARGET_* only matters to the TCC sources; on a target TCC has no
        -- backend for, the whole backend is left out and the engine runs the
        -- interpreter (the public API stays, it just reports no JIT).
        add_defines(tcc_defs)
        add_defines("WHALE_JIT_AVAILABLE")
    else
        -- Without TCC the JIT translation unit still has to exist (the engine
        -- calls its entry points), but everything inside is compiled out; see
        -- whale_jit.c's WHALE_JIT_AVAILABLE guard.
        add_defines("WHALE_JIT_OFF")
    end
    add_includedirs(SRC, TCC, TCC_RT)
    add_defines(qjs_defines)
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

-- WebAssembly 扩展：独立的静态库 libWhaleJS-wasm.a，基于 wasm3。
-- 它只 include 公开头文件（quickjs.h / whalejs.h / wasm3.h），不碰引擎内部，
-- 所以不链它时引擎行为完全不变；链上之后调用 JS_WhaleInstallWasm(ctx) 即可
-- 在 context 上挂出 globalThis.WebAssembly。
-- 注意：产物名必须与 whalejs_static 区分开（同名 target 会被 xmake 从链接行
-- 上丢掉，见 notes），所以 target 名是 WhaleJS-wasm、basename 是 WhaleJS-wasm。
local WASM3 = "thirdparty/wasm3/source"
local wasm3_sources = {
    WASM3 .. "/m3_bind.c", WASM3 .. "/m3_code.c", WASM3 .. "/m3_compile.c",
    WASM3 .. "/m3_core.c", WASM3 .. "/m3_env.c", WASM3 .. "/m3_exec.c",
    WASM3 .. "/m3_function.c", WASM3 .. "/m3_info.c", WASM3 .. "/m3_module.c",
    WASM3 .. "/m3_parse.c", WASM3 .. "/m3_validate.c", WASM3 .. "/m3_xxh64.c",
    WASM3 .. "/m3_deterministic.c",
}

target("WhaleJS-wasm")
    set_kind("static")
    set_basename("WhaleJS-wasm")
    -- 绑定层 + wasm3 本体。wasm3 不启用 WASI/uvwasi（需要额外依赖），
    -- 只用核心解释器：WASI 的 host 函数由嵌入方通过 imports 提供。
    add_files(SRC .. "/whale_wasm.c")
    add_files(SRC .. "/whale_wasm_c.c")
    add_files(SRC .. "/whale_wasm_rt.c")
    add_files(wasm3_sources)
    add_includedirs(SRC, WASM3, { public = true })
    add_defines(qjs_defines)
    -- 中断支持靠 gas 计量：runtime 可挂起 + 计量后，wasm3 在每条控制流转折
    -- 点（循环回边、函数入口）切出安全点，gas 用尽就在那里暂停，宿主可以
    -- 恢复。stack switching / snapshots 是挂起机制的前提（见 m3_config.h），
    -- typed refs 又是 stack switching 的前提。exception handling 和 memory64
    -- 用不到，关掉。
    add_defines("d_m3HasTypedRefs=1", "d_m3HasStackSwitching=1",
                "d_m3HasSnapshots=1", "d_m3HasGasMetering=1",
                "d_m3HasExceptionHandling=0", "d_m3HasMemory64=0")
    if is_win() then
        add_defines("WIN32_LEAN_AND_MEAN", "_WIN32_WINNT=0x0601")
    end
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

-- WebAssembly 扩展的单元测试：xmake run whale_wasm_test
-- 单独一个 target，因为 tests/whale 下的文件都链进 whale_test，而那个测试
-- 不链接 wasm 扩展。
target("whale_wasm_test")
    set_kind("binary")
    set_rundir("$(projectdir)")
    -- Pure C, like the whole project: no C++ runtime and no static
    -- initialisers in the test binary.
    set_languages("c11")
    add_deps("whalejs_static", "WhaleJS-wasm")
    add_files("tests/wasm/*.c")
    add_includedirs(SRC, WASM3)
    add_defines(qjs_defines)
    -- No --export-all-symbols here: the test does not need to export anything,
    -- and exporting every symbol makes the loader process a large export table
    -- for no benefit.
    link_whale(false)

-- WebAssembly 扩展的嵌入示例：只 include 公开头文件，链接两个库。
target("whale_wasm_demo")
    set_kind("binary")
    set_rundir("$(projectdir)")
    add_deps("whalejs_static", "WhaleJS-wasm")
    add_files("examples/wasm.c")
    add_includedirs(SRC)
    add_defines(qjs_defines)
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

-- Promise hook 测试：xmake run whale_promise_test
-- 单独一个 target，因为 tests/whale 下的文件都链进 whale_test，而那个测试自己
-- 有 main()。
target("whale_promise_test")
    set_kind("binary")
    set_rundir("$(projectdir)")
    set_languages("c11")
    add_deps("whalejs_static")
    add_files("tests/promise/*.c")
    add_includedirs(SRC)
    add_defines(qjs_defines)
    link_whale(false)

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
