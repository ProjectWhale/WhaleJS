# WhaleJS 架构（内部文档）

## 目标
用 QuickJS-ng + TinyCC 做一个支持 **JIT / AOT** 的纯 JS 引擎。
JIT 要求真编译成机器码（TinyCC 后端），不是把解释器循环包一层。

WebAssembly 是一个**独立的扩展**（`libWhaleJS-wasm.a`），不是引擎的一部分：
不链它时引擎不认识 wasm，链上之后由宿主调用一次挂载函数。

## 目录
| 目录 | 用途 |
|------|------|
| `src/` | 改造版 QuickJS 源码（起点 = QuickJS-ng master，0.16.2） |
| `src/whale_wasm*.c` | WebAssembly 扩展：绑定层、wasm→C 翻译器、运行期辅助 |
| `src/tcc/` | TinyCC 0.9.27（JIT 后端） |
| `thirdparty/wasm3/` | wasm3 0.9.2（扩展的解释器与模块解析） |
| `doc-internal/` | 内部实现与接口文档（本目录） |
| `notes/` | 长期问题与经验，见 `notes/index.md` |
| `temp/` | 临时数据（下载物、baseline 构建目录），**不入 git** |

## 构建
```powershell
xmake f -y -p mingw -m release   # 必须带 -p mingw：本机没有 MSVC，否则报 "toolchain not found!"
xmake -y
.\build\mingw\x86_64\release\whalejs.exe -e "console.log(1+2)"
```
- xmake 3.0.9；本机平台名是 **mingw**（不是 windows），`is_plat("windows")` 为 false，判平台用 `xmake.lua` 里的 `is_win()`。
- MinGW 链接参数对齐 quickjs-ng 的 cmake MINGW 分支：`-static -static-libgcc -Wl,--stack,8388608` + `-lpthread`。
  少了这些会报 `cannot find -ldl`（走错非 Windows 分支）或运行时缺 winpthread。
- 引擎本体是 C（C11）；自有新代码也用 C，**不使用 C++**（rules/user.md）。

### 平台与 JIT 支持
TCC 的目标宏由 `xmake.lua:tcc_target_defines()` 按构建目标给出，不再写死：

| 平台 | 架构 | TCC 目标宏 | JIT |
|------|------|-----------|-----|
| Windows / MinGW | x86_64 | `TCC_TARGET_PE` + `TCC_TARGET_X86_64` | 是 |
| Windows / MinGW | arm64 | `TCC_TARGET_PE` + `TCC_TARGET_ARM64` | 是（PE 后端从 TCC mob 分支移植） |
| Linux / macOS | x86_64 | `TCC_TARGET_X86_64` | 是 |
| Linux / macOS | arm64 | `TCC_TARGET_ARM64` | 是 |
| Linux | arm (32) | `TCC_TARGET_ARM` + EABI/HARDFLOAT | 是 |
| 其它 | — | — | **否**：不编译 TCC，`WHALE_JIT_OFF` 走桩实现，解释器照常跑 |

`WHALE_JIT_AVAILABLE` 只在 TCC 有该目标后端时定义。关闭 JIT 时 `whale_jit.c`
编译成一组返回"没有 JIT"的桩函数，公开 API 与语义（等同 `JS_JIT_OFF`）不变，
所以同一份嵌入方代码在每个平台都能构建运行。

## 产物
| 产物 | 内容 |
|------|------|
| `libwhalejs.a` | 引擎本体：JS 运行时 + 标准库 + JIT/AOT，不认识 wasm |
| `whalejs.dll/.so` | 同上，动态库形态 |
| `libWhaleJS-wasm.a` | WebAssembly 扩展（wasm3 + 绑定层 + 翻译器） |
| `whalejs(.exe)` | CLI，手工验证与 perftest 用 |

## WebAssembly 扩展
扩展是**单独编译的静态库**，只 include 公开头文件（`quickjs.h` /
`whalejs.h` / `wasm3.h`）。不链它时引擎行为与之前完全一样。

- 挂载：`JS_WhaleInstallWasm(ctx)`（内部先 `JS_WhaleInstallWasmRuntime(rt)`），
  之后 context 上就有 `globalThis.WebAssembly`。两者都幂等。
- 实现要点：
  - `Module` 只保存一份字节副本；`instantiate()` 每次重新 parse 进新的
    wasm3 environment，所以一个 Module 可以实例化多次。
  - Instance / 导出函数 / memory 视图共享一个引用计数的 `WasmCore`，
    三者可以任意顺序被 GC 回收，不产生 JS 引用环。
  - 导入用一个泛型跳板：wasm3 把 `IM3Function` 交给它，参数类型在调用时
    读，不需要为每种签名写一个 C 函数。
  - `memory.buffer` 是 wasm3 线性内存上的零拷贝 ArrayBuffer。`memory.grow`
    会 realloc，所以旧 buffer 被 detach、下次访问发新的（每次导出调用后都
    检查一次，窗口是一次调用而不是一次 GC）。
  - 导出面：`validate` / `compile` / `instantiate` / `Module` / `Instance`，
    两个构造器都带真正的 `prototype` 对象并登记为对应 JSClass 的原型。
    这一点是必须的：esbuild 的参数校验会对每个参数做
    `x instanceof WebAssembly.Module`，没有 `prototype` 时 `instanceof`
    直接抛异常，而不是返回 false。
- **已知与规范的差异**：`WebAssembly.compile()` 返回的是 Module 本身，不是
  `Promise<Module>`。真实加载器（vite 的 wasm 插件就是
  `WebAssembly.compile(bytes).then(WebAssembly.instantiate)`）依赖它是
  thenable。改成异步是契约变更，会影响现有嵌入方与测试，所以留作显式决策
  而不是顺手改掉。
- 构建：`xmake build WhaleJS-wasm`；测试 `xmake run whale_wasm_test`；
  示例 `xmake run whale_wasm_demo`。

### 与 JIT 共享 TCCState 的约定
引擎的 TCCState 是**进程级单例**：`tcc_new()` 在 state 已存在时直接返回它，
`tcc_delete()` 对它是空操作。原因是 `tcc_cleanup()` 会重置所有 state 共享的
`sym_pools`/`sym_free_first`，第二次创建会让已经编译好的代码全部失效（实测
表现为第二个 `JSRuntime` 之后 `tcc_get_symbol()` 在 `strcmp` 里崩）。

由此带来两条约束，写扩展时必须遵守：

1. **relocate 会搬动整个 state 的代码**，之前取到的所有函数指针立刻失效。
   计数器 `tcc_reloc_generation`（`JS_WhaleJitGeneration()`）每次
   `tcc_relocate()` 递增；缓存地址的一方要连同代号一起记，发现不一致就重新
   `JS_WhaleFindSymbol()`。**不要**维护"所有实例"的全局链表去批量重绑——
   实例可能属于已经被销毁的 `JSRuntime`，那个链表正是这样变成悬空指针的。
2. **编译单元要自己开合**：`tccelf_begin_file()` / `tccelf_end_file()`，并在
   每次 relocate 前后清 `runtime_main`（`JS_WhaleTccClearRuntimeMain`）。
   否则 `pe_output_file()` 会把上一次的入口符号当成"这是个程序"，在下次
   relocate 时注入无法解析的 `__runmain`，编译直接失败。

### 两条执行路径
每个导出函数按函数体各走各的：

1. **翻译路径**（快）：`src/whale_wasm_c.c` 把 wasm 函数体翻译成 C，交给
   引擎**唯一的那个** TCCState 编译（`JS_WhaleCompileExtern`）。
2. **wasm3 路径**（兜底）：翻译器不接受的函数体继续由 wasm3 解释。

翻译器遇到没实现的 opcode 就放弃**那一个函数**，不是整个模块。所以正确性
从不依赖翻译器的覆盖率，只有速度依赖。

### 翻译器（wasm → C → TCC）
- **只做翻译，不做前端优化**：wasm 已经优化过，翻译器只负责把栈机变成 C
  局部变量、把结构化控制流变成 `{}`/`for(;;)`/`if` + `goto`。
- **两遍扫描**：C 要求声明在语句前，而临时变量个数要翻译完才知道，所以第一
  遍只数不写，第二遍才发 token。
- **函数签名**：生成的 C 是
  `ret f(<typed params>, unsigned char *mem, unsigned int memsize,
  unsigned long long *glob)`。
  - `mem`/`memsize` 是线性内存，按参数传而不是全局，多个实例共存才正确。
  - `glob` 是这一实例的全局变量数组（i32/f32 存在低 32 位）。
  - 参数用**真实 C 类型**传递（`unsigned int` / `unsigned long long` /
    `float` / `double`），不是统一 64 位槽：ABI 不同，混用会让 double 落进
    整数寄存器。
- **统一入口 wrapper**：每个翻译过的函数还额外生成一个
  `void f__w(unsigned long long *a, unsigned long long *r,
  unsigned char *mem, unsigned int memsize, unsigned long long *glob)`。
  绑定层只按这一种 ABI 调用，所以 0–8 个参数、任意类型组合都不需要枚举签名
  （之前手写的分派表只覆盖到 2 个参数，3 个以上会掉回 wasm3）。浮点按位
  拷贝进出槽位，不做数值转换；整型按声明宽度截断。
- **批量 relocate**：relocate 会重排整个 TCC state 里的代码，之前拿到的函数
  指针全部失效。所以一个模块的所有函数编译完只 relocate 一次，然后把地址
  一次性取回；**并且**所有已存在的实例都要重新解析自己的符号
  （`wasm_rebind_all`），否则第二次 `instantiate` 会让第一个实例的导出指向
  已失效的代码。导出函数每次调用都从表里读地址，不缓存到描述符里。
- **不动点翻译**：`call` 只能指向已经翻译好的同模块函数（原生代码没有回解释
  器的重入路径），而被调者可能在模块里更靠后。绑定层因此反复扫描，每轮至少
  多翻译一个函数，直到不再有进展。
- **全局变量**：翻译代码读写的是一份实例私有的 `unsigned long long[]` 影子
  数组；每次跨入/跨出原生代码时和 wasm3 的 `M3Global` 同步一次，所以同一
  模块里"部分函数翻译、部分仍解释"也能看到一致的值。
- **覆盖**：i32/i64/f32/f64 全套数值与比较、数值转换与 reinterpret（含
  trapping / saturating 截断）、局部变量、全局变量、block / loop / if / br /
  br_if、`drop` / `select`、直接 `call` 与 `return_call`、线性内存 load/store
  （含边界检查）、`memory.size`、`return`、`unreachable`。
- **不覆盖**（这些函数留给 wasm3）：`call_indirect`、`br_table`、
  `memory.grow`、SIMD、异常、多返回值、import 调用。
- **TCC 的 token 流只有内置类型关键字**：`uint32_t` 这类 typedef 名解析不
  到（不读头文件），所以生成代码里写 `unsigned int` / `unsigned long long`。
  同理 `>>`/`<<` 这类组合 token 也不能直接发，`memory.size` 用除以 65536
  代替右移。

### 中断
wasm 执行期间不回引擎，所以 `JS_SetInterruptHandler` 默认收不到任何东西，
死循环会把线程卡死。两条路径都要处理。

**wasm3 路径**（0.9.2 的 gas 计量 + 可挂起 runtime）：
- runtime 在 `instantiate` 时 `m3_SetSuspendable(rt, true)`，并且**在编译前**
  武装 gas（`IsMetering` 是编译期判断，`m3_FindFunctionIn` 又是惰性编译，
  晚于第一次查找再设 gas 的话代码里根本没有检查点）。
- 调用因 gas 用尽而返回 `m3Err_continuationSuspended` 时，绑定层调用
  `JS_WhalePollInterrupt(ctx)`（引擎新增的公开入口，因为 handler 藏在私有
  `JSRuntime` 里）：要求停就抛错，否则补一份 gas 继续 resume。
- 停下的实例会被标记 retire：那次调用留下一个挂起的 continuation 占着
  runtime 的值栈，不能原地丢弃，也不能继续跑（那正是被要求停下的代码）。
  之后再调用它直接报错，`m3_FreeRuntime` 在 finalizer 里回收。

**翻译路径**：
- 每个循环回边生成一次 `whale_wasm_check_interrupt()`。它是原生代码，引擎的
  中断处理器没有任何调用点可以到达，所以这是唯一的位置。
- 这个调用是**摊薄**的：内部计数器每 4096 个回边才真正问一次处理器。逐回边
  调用实测让 1MB 求和从 202ms 变成 259ms（比 wasm3 还慢），摊薄后回到 114ms。
- 要求停时走 trap 路径并置 `whale_wasm_interrupted()`，绑定层据此报
  `InternalError: interrupted` 而不是"越界/除零"。

两条路径抛的都是引擎自己的 `InternalError: interrupted`，**不可 catch**，
和解释器中断一致。

### 性能（实测，同一台机器）
`temp/wasm3build/tbench_single.c` 的 A/B（`WHALE_WASM_NOTRANSLATE=1` 强制走
wasm3）。测量必须在**同一个 JSRuntime** 里做：引擎的 TCCState 是进程级的，
第二个 `JS_NewRuntime` 之后再 relocate 会让符号表失效（既有问题，与 wasm
无关，纯 JS JIT 也能复现）。

| 用例 | wasm3 | 翻译器 | |
|------|-------|--------|---|
| memsum 1MB ×20 | 190 ms | **137 ms** | 1.39× |
| LCG 1M ×20 | 159 ms | **109 ms** | 1.46× |

perftest 形状（单次调用 + 长循环，无跨调用缓存）：

| 用例 | WhaleJS JIT | wasm3 |
|------|------------|-------|
| `arith` 20M（`s=s*c+k`） | 62–78 ms | 151 ms |
| `loop_sum` 5M（整数 + `%`） | 49–60 ms | 111 ms |

结论：**wasm3 是解释器，不是 codegen**。wasm 字节码本身确实被优化过，但
wasm3 把它编成 op 函数指针序列再逐条执行，损失的是编译期优化而不是输入
质量。翻译器把它变成真正的 C，收益就是上面那 1.5 倍；要再往 JIT 那列靠，
需要的是后端质量（TCC 的指令选择），不是翻译质量。

测量注意：**不要用 fib 这类"参数少、纯数值"的函数做基准**——引擎的 memo
会把结果缓存住（`fib(30)` 未被缓存时 779 ms，被缓存后 12 ms，差 65 倍）。
perftest 里 `arith`/`loop_sum` 的第二次调用同样是 0 ms。用单次长循环、或者
参数变化的形状才能测到真实执行。

## 不变量
- 保持与上游 QuickJS-ng 的兼容：改动尽量收在 hook 点（如 `JS_CallInternal` 入口、`JSFunctionBytecode` 附加字段），便于后续跟上游。
- JIT 必须可编译期/运行期关闭，关闭后行为等同原版。
- wasm 扩展不参与引擎构建；引擎源码对它零依赖。
