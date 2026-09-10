# <1>: 环境与第三方源码获取
TIPS

## 工具链（详见 temp/environment.md）
xmake 3.0.9、gcc 16.1.0 (MinGW-w64, x86_64-win32-seh)、cmake 4.4.0-rc2、mingw32-make、python 3.12、
node 24。缺失：MSVC/clang/ninja/tcc/docker/cargo。本机 shell 是 PowerShell（不是 bash）。

## 源码获取（踩坑记录）
- `git clone https://github.com/quickjs-ng/quickjs` 在 PowerShell 里连 github:443 超时（curl 28）。
  可用替代：`https://codeload.github.com/<owner>/<repo>/zip/refs/heads/<branch>` 直接下 zip 再 Expand-Archive。
- TinyCC 官网 https://bellard.org/tcc/ 只给文档链接；实际二进制/源码在
  https://download.savannah.nongnu.org/releases/tinycc/ ，最新源码是 **0.9.27**（没有 0.9.28）。
- 目录约定：`.gitignore` 已排除 temp/ 与 build/；第三方源码最终落 thirdparty/（见 <2>）。

## baseline 验证
原版 QuickJS-ng 0.16.2 用 cmake + MinGW Makefiles 编译通过，`qjs.exe -e "console.log(...)"` 输出正常，
`--version` 报 0.16.2。命令见 temp/qjs-build（cmake 生成，产物在 temp/ 内，不入 git）。

# <2>: 目录结构：src/ + thirdparty/，xmake 平台名是 mingw
TIPS

- QuickJS-ng 源码从 temp/ 迁到 **src/**（我们的改造对象）；TinyCC 放 **thirdparty/tinycc/**。
  temp/ 只留下载物与 temp/environment.md。
- **xmake 平台坑**：本机 xmake 平台名是 `mingw`，不是 `windows`，`is_plat("windows")` 为假。
  xmake.lua 第一版因此在 Windows 上走了非 Windows 分支 → `add_syslinks("dl")` → 链接报
  `ld.exe: cannot find -ldl`。修法：`is_win() = is_plat("windows") or is_plat("mingw")`，
  链接参数对齐 quickjs-ng cmake 的 MINGW 分支（`-static -static-libgcc -Wl,--stack,8388608 -lpthread`）。
- 构建产物路径：`build/mingw/x86_64/release/qjs.exe`（平台名同样体现为 mingw）。
- **构建已通过**：`xmake f -y -p mingw -m release` + `xmake -y` 成功；`qjs.exe -e` 输出正常。
  不带 `-p mingw` 时 `xmake f` 会报 `target(qjs): toolchain not found!`（本机没装 VS）。
- 性能基线（解释器，JIT 前的对照）：`fib(30) = 832040`，**253ms**。
- src/ 精简：移除 test262/、.github/、docs/、run-test262.c、fuzz.c、qjs-wasi-reactor.c、
  meson.build/meson_options.txt、unicode_download.sh、update-version.sh、.gitmodules；src 现 3.5MB。

# <3>: 工具使用坑：对源码/构建文件落笔后，本回合 bash 会被拒
TIPS

- 宿主规则：一旦本回合用 file tool（write_file/edit_file）改过 `xmake.lua` 或源文件（如 `src/quickjs.c`），
  本回合剩下来的 **bash 调用全部被拒**（提示 `bash cannot declare which files it changes while a
  read-evidence requirement is outstanding: <path>`），连只读命令也拒；**下一个用户回合自动恢复**。
  → 对策：验证性命令（编译、跑测试、git commit）安排在同一回合的**前半段**执行，文件落笔放后半段；
  或者干脆下回合再验证。
- bash 无法直接把多行内容写进源文件（会被上面这条拦），改源码只能走 `edit_file`，且**每次编辑前要重新
  read 目标窗口**：同一回合内对同一文件连续两次 edit 会报 `file changed after read`/`missing lines a-b`。
  实践节奏：read 窗口 → edit 一处 → read 窗口 → edit 下一处。

# <4>: Step 3 —— whalejs 接口与单元测试
SOLVED

- 接口 `src/whalejs.h`（新增，只加不改上游 `quickjs.h`）：`JSJitMode`(OFF/LAZY/AOT)、`JSWhaleJitStats`、
  `JSWhaleContext`（内嵌状态，零分配）、`JS_WhaleSetJitMode/GetJitMode/GetJitStats`，以及引擎内部
  `JS_WhaleNoteJitRequest/NoteJitResult`。新建 context 默认 `JS_JIT_LAZY`。
- `src/quickjs.c` 侵入点只有 4 处（上游升级时重放这 4 处即可）：include `whalejs.h`；
  `struct JSContext` 末尾加 `JSWhaleContext whale;`；`JS_NewContext` 里设默认模式；
  `JS_SetContextOpaque` 之后追加实现块。
- 测试：`tests/whale/test_api.c`（无框架 CHECK → exit code）、`tests/whale/fib.js`（fib(30) 语义+耗时）。
  构建目标 `whale_test`，跑法 `xmake run whale_test`。
- 验证（commit 22c3a46）：`xmake -y` build ok；`xmake run whale_test` → `whale api test: ok`；
  `qjs tests/whale/fib.js` → `fib(30)=832040 244ms`（仍是解释器，与 baseline 253ms 同级）。
- 待优化：xmake 里 `qjs` 与 `whale_test` 各编一份引擎源码，全量构建 ~75s；target 变多后抽
  `whalejs_lib` 静态库共享。

# <5>: Step 4 —— JIT 挂接层（热点统计）
SOLVED

- `src/whalejs.h`：加 `WHALE_JIT_HOT_THRESHOLD`（默认 100，可用 `-D` 覆盖）。
- `JSFunctionBytecode` 末尾加两个字段：`uint32_t whale_call_count`、`void *whale_jit_entry`。
  **零初始化已查证**：Bytecode 只有两处分配，均为 `b = js_mallocz(ctx, function_size)`——
  编译路径 37108、反序列化 `JS_ReadFunctionBytecode` 路径 39812，所以字段天生为 0。
  （`function_size = sizeof(*b)`，新字段自动计入分配大小。）
- 挂接点：`JS_CallInternal` 里 `ctx = b->realm;` 之后调 `whale_on_call(ctx, b)`。
  协程恢复走 `goto restart`，会跳过这个 hook（第一版可接受）。
- `whale_on_call()`（static，定义在实现块里，早于 18240 的调用点）：`JS_JIT_OFF` 立即 return（零开销）；
  否则 `stat_requests++`、`whale_call_count++`；达阈值且尚无 `whale_jit_entry` 时进 hot 分支——
  该分支目前是空的，下一步在这里调 TinyCC 产出机器码。
- 测试新增两条：JIT=OFF 评估三段 JS 后 `requests` 纹丝不动；JIT=LAZY 评估后 `requests` 增长且结果与 OFF 一致。
- 验证（commit bdd65de）：build ok 69.5s；`whale api test: ok`；`fib(30)=832040 236ms`（挂计数后与 244ms 同级，
  说明 hook 开销可忽略）；`fib(25)=75025`。
- 下一步：在 MinGW 下构建 libtcc（thirdparty/tinycc），把 hot 分支换成真实编译。

# <6>: Step 5 —— TinyCC 接入（libtcc 从源码构建 + 运行期编译跑通）
SOLVED

- **TCC 的 configure 跑不了**：它是 shell 脚本，本机（Windows/PowerShell）没有 sh。
  等价配置手工落库到 `thirdparty/tcc-runtime/config.h`（内容抄 `win32/build-tcc.bat` 生成的那几行）：
  `TCC_VERSION "0.9.27"`，`TCC_TARGET_X86_64` → `TCC_LIBTCC1 "libtcc1-64.a"`。
- **libtcc 只有一个翻译单元**：`tcc.h:293` 里 `ONE_SOURCE` 默认 1，`libtcc.c` 把所有后端 .c 都 include 了。
  编译宏对齐 build-tcc.bat：`-DTCC_TARGET_PE -DTCC_TARGET_X86_64`；再 `-Ithirdparty/tcc-runtime`
  让它找到 config.h。实测 libtcc.o 319,954 字节。
- **运行时支持库要可寻址**：`thirdparty/tcc-runtime/{include,lib}`（取自预编译包
  `tcc-0.9.27-win64-bin.zip`），lib 里是 `libtcc1-64.a`（+32 位版）与 `kernel32.def`/`msvcrt.def`/
  `gdi32.def`/`user32.def`。共 87 文件 1.29MB。根 `.gitignore` 的 `*.a` 会吃掉 libtcc1，
  已加例外 `!thirdparty/tcc-runtime/lib/*.a`。
- **踩坑：`tcc_relocate` 报 `library 'libtcc1-64.a' not found`**。只调 `tcc_set_lib_path` 不够——
  TCC 的 include/lib 搜索路径在 `tcc_new()` 时就定下来了。正确顺序：
  `tcc_set_lib_path` → `tcc_add_include_path` → `tcc_add_library_path` → `tcc_set_output_type(TCC_OUTPUT_MEMORY)`。
- 验证：临时 smoke（temp/，未入库）输出 `tcc jit: add(2,3)=5 fib(20)=6765`（exit 0）。
  入库版本是 `tests/whale/test_api.c` 里的 `check_tcc_backend()`，由 `whale_test` 覆盖；
  xmake 里新增 `libtcc` 静态库 target，`qjs`/`whale_test` 都 `add_deps("libtcc")`。
- **第二个坑：`xmake run` 的 cwd 不是项目根**。测试里用相对路径 `thirdparty/tcc-runtime` 会找不到
  libtcc1-64.a（`tcc: error: library 'libtcc1-64.a' not found`），接着 relocate 失败还会踩崩
  （exit -1073741819）。修法两条：xmake 里给两个 binary target 加 `set_rundir("$(projectdir)")`；
  测试里 compile/relocate 失败就 `tcc_delete` + 提前 return。
- 最终验证（commit d648385）：`xmake -y` build ok（增量 1.45s，全量约 20s 含 libtcc）；
  `xmake run whale_test` → `whale api test: ok`，exit 0（含 check_tcc_backend 的 add/fib 断言）；
  `git ls-files` 确认 libtcc1-32.a / libtcc1-64.a 已入库（93 个文件）。
- 下一步：`whale_on_call` 的 hot 分支里把 QuickJS 字节码翻成 C 源码 → `tcc_compile_string` →
  `tcc_relocate` → 填 `b->whale_jit_entry`，第一版只支持无捕获的纯计算函数（fib 那类）。

# <7>: Step 6 前哨 —— JIT 目标代码 ABI 原型跑通
SOLVED

原型：`temp/jit_proto.c` + `temp/jit_target_src.inc`（手写目标代码，未入库）。结果：
```
jit fibi(10)=55  interpreter=55  MATCH     exit 0
```
即 **TCC 运行期编译出的 C 代码可以直接操作真实 `JSValue`**，并通过 `tcc_add_symbol` 回调引擎侧运行时，
结果与解释器逐位一致。生成器照抄这个形状即可：
- 值栈是函数内局部数组 `JSValue st[b->stack_size]`（C 栈上，每槽持有引用）；
- 每个 opcode 摊成一次 `whale_jit_*(ctx, &s, ...)` 调用（运行时在引擎侧由 gcc -O3 编译，TCC 只串调用序列）；
- 跳转用真实 C 标签 `L<bytecode 偏移>`；出口 `return st[--s.sp]`（转移所有权）；
  异常走 `exc:` 清栈 + `return JS_EXCEPTION`。

**坑一：quickjs-ng 的公开头没有算子 API**。上游 quickjs.h 有 `JS_Add/JS_Sub/JS_Lt`，ng 版**没有**
（`quickjs.h` 里搜不到），所以 JIT 运行时要么用内部 `js_add_slow` 一类，要么自己写数值快路径——
后者正好是提速抓手（int+int 直接算，不分配、不碰 GC）。公开头里能用的：
`JS_MKVAL / JS_VALUE_GET_TAG / JS_VALUE_GET_INT / JS_TAG_INT / JS_TAG_IS_FLOAT64 /
JS_NewInt32 / JS_NewFloat64 / JS_NewBool / JS_DupValue / JS_FreeValue / JS_ToBool / JS_ToFloat64 /
JS_ThrowTypeError`。

**坑二：PowerShell 里 `-Wl,--stack,8388608` 必须加引号**，否则被当数组参数 →
`参数列表中缺少参数`。gcc 直接命令行编译时的其它参数（`-DWIN32_LEAN_AND_MEAN` 等）同理建议加引号。

读字节码 dump：debug 构建 + `$env:QJS_DUMP_FLAGS="1"`（`JS_DUMP_BYTECODE_FINAL=0x01`），或 `qjs -D 1`。

## 第一版目标函数（迭代 fib）与其字节码
`function fibi(n){var a=0,b=1;for(var i=0;i<n;i++){var t=a+b;a=b;b=t;}return a;}`
```
 0: push_0; put_loc0 a          11: get_loc0_loc1; add; put_loc3 t
 2: push_1; put_loc1 b          15: get_loc1 b; put_loc0 a
 4: push_0; put_loc2 i          17: get_loc3 t; put_loc1 b
 6: get_loc2 i; get_arg0 n      19: inc_loc 2; goto8 ->6
    lt; if_false8 ->22          22: get_loc0 a; return
```
需要的 opcode（12 个）：`push_0..7`、`put_loc0..3`、`get_loc0..3`、`get_loc0_loc1`、`get_arg0`、
`lt`、`add`、`inc_loc(loc8)`、`if_false8`、`goto8`、`return`。

## 解释器语义（已逐条核对源码，必须照抄）
- `goto8`：`pc += (int8_t)pc[0]`（此时 pc 指操作数）→ **目标 = opcode 偏移 + 1 + offset**。
- `if_false8`：pop 一个值做真值判断（tag ≤ JS_TAG_UNDEFINED 时直接取 int，否则 `JS_ToBoolFree`，
  **会 consume 该值**），为假则 `pc += (int8_t)pc[-1] - 1` —— 同样等于 opcode+1+offset。
- `put_locN`：`set_value(ctx, &var_buf[N], *--sp)`（free 旧值、接管新值）。
- `inc_loc idx`：int 快路径 `+1`；`INT32_MAX` 时退浮点。
- `return`：`ret_val = *--sp`。
- `get_loc0_loc1`：`*sp++ = js_dup(var_buf[0]); *sp++ = js_dup(var_buf[1]);`

## 第一版 JIT 支持面（不满足就放弃，回退解释器）
`func_kind == JS_FUNC_NORMAL`、无 eval/with、无 `get_var_ref/put_var_ref`（闭包）、**无 `call`**
（递归版 fib 用 `call1` + `get_var_ref0`，第一版不覆盖）、无 `push_this/new_target/super`。

# <8>: Step 6 —— 第一版 JIT 跑通（功能 ✓ / 性能未达标）
SOLVED（功能）+ UNRESOLVED（性能）

## 落地了什么
- `src/quickjs.c` 末尾新增 JIT 区块（`libtcc.h` 就在这一区 include）：
  - `whale_jit_gen()`：pass 1 扫字节码收跳转目标 + 拒绝不支持的 opcode；pass 2 生成 C 源码。
    支持 12 类 opcode（对应 <7> 里 dump 出来的清单）。
  - **原生优先**：int 快路径内联展开 —— `JS_VALUE_GET_TAG(...) == JS_TAG_INT` 位测试 + 一条机器加法
    （溢出转 double），不调 helper、不分配、不碰 GC；生成的宏（`WHALE_ADD/SUB/MUL/LT/GET_LOC/PUT_LOC/
    GET_ARG/DROP/DUP_TOP/INC_LOC/IF_FALSE`）全部写成**单行**，省得跟续行反斜杠打架。
  - **退化路径**：快路径不成立就调 helper，helper 直接复用解释器实现
    （`js_add_slow` / `js_binary_arith_slow(OP_sub|OP_mul)` / `js_relational_slow(OP_lt)` /
    `js_unary_arith_slow(OP_inc)`）—— 等于把那条 opcode 的 switch 分支搬过来，语义必然一致。
  - `whale_jit_compile()`：`tcc_compile_string` → `tcc_relocate(TCC_RELOCATE_AUTO)` → `tcc_get_symbol`；
    入口挂 `b->whale_jit_entry`，TCCState 挂新增字段 `b->whale_jit_tcc`（机器码在它身上，不能提前释放）。
    生成的代码 `#include "quickjs.h"`（TCC include 路径要加 `src/`），并且
    **`JS_DupValue / JS_FreeValue / JS_ToBool` 必须 `tcc_add_symbol` 注册**，否则 TCC 报 undefined symbol。
- 挂接：`whale_on_call` 过阈值编译一次（新增 `whale_jit_tried` 防重试，失败静默回退）；
  `JS_CallInternal` 里 `b->whale_jit_entry` 非空就直接调原生入口，并复刻 `done:` 收尾
  （`close_var_refs` + 释放 `local_buf..stack_buf` + 恢复 `rt->current_stack_frame` + 直接 return）。
- 测试：`tests/whale/test_api.c` 第 9 段 —— 热点 `fibi(30)` 必须 `compiled >= 1` 且结果仍是 832040。

## 结果
- 功能：`whale api test: ok`（含 JIT 断言）；400× `fibi(30)` 全部 832040。
- 性能：**没提速**。3000× `fibi(30)`：解释器 ~10ms，JIT ~16ms。
  两条原因：① TCC 基本不优化（单遍编译器，没有寄存器分配优化）；
  ② 生成代码里值栈 `st[]`/`sp` 还在内存里，每条 opcode 都要 load/store。
- 回退路径顺带验证了：注册符号前 TCC 编译失败 → 静默回退解释器 → 结果依然正确。
- commit 9581e82。

## 下一步优化（按收益排序，待实现）
1. **栈到寄存器提升（最关键）**：JS 字节码在每个位置的栈深度是静态确定的，可以把 `st[k]` 直接映射成
   C 局部变量 `v0, v1, ...`（生成器跟踪 sp；跳转目标处 sp 不一致就放弃 JIT）。这样 fibi 的循环变成
   纯局部变量运算，TCC 也能把 a/b/i/t 放进寄存器。**这是从"少一层 switch"变成"真原生"的关键一步。**
2. **类型特化 / 去装箱**：若参数与局部变量在所有路径上都是 small int，整段循环可以在 int64 域裸算，
   只在出入口与 JSValue 互转（需要类型推断 + 去优化回退）。
3. **递归展开**：先支持 `call`（回调查引擎，需要 `get_var_ref` 一起做）；再对"直接自递归 + 尾递归"
   做循环化/展开（`fib(n-1)+fib(n-2)` 这类可展开成两层再调自身一次）。
4. **常量折叠 / 代数化简 / 循环不变式外提**：生成器里对 `push_const + op` 先折叠；
   循环里不变的量（如参数 `n`）提到循环外。
5. **TCC 侧优化**：`tcc_set_options(s, "-O2")` 可试（TCC 优化很弱，别期待太多）；
   真要快可以考虑离线 gcc -O3 做 AOT 路径。
6. **编译缓存与回收**：目前每个 JIT 过的函数泄漏一个 TCCState（代码里有 `ponytail:` 标注），
   编译开销 ~ms 级；后续做磁盘缓存 + 在 `free_function_bytecode` 里 `tcc_delete`。
7. 老账：`whale_on_call` 里那条过时注释（"下一步在这里调 TinyCC"）待清理；
   xmake 里 `qjs`/`whale_test` 各编一份引擎源码，可抽 `whalejs_lib` 静态库。

# <9>: Step 6 —— 栈槽提升为局部变量：做了，但没提速（诊断清楚）
UNRESOLVED（性能）

## 做了什么（commit d019316）
- `whale_jit_gen()` 现在跟踪**编译期栈深度**，把 `st[k]` 直接发成 C 局部变量 `v0..vN`；
  跳转目标若以不同深度到达就放弃 JIT 编译（说明字节码里有点我们没建模的东西）。
- 语义零变化：值还是完整 `JSValue`，每次算术前照样重新判 tag，**没有做任何"假设它是 int"的特化**——
  JS 是弱类型，那种特化必须配 guard + 去优化，留到下一步单独做。

## 测得的数据（fibi(30)，每调用纳秒）
| 实现 | ns/call |
|------|---------|
| 原版 QuickJS-ng 0.16.2（纯解释器，gcc -O3 编） | **2200** |
| WhaleJS + TCC JIT | **3200** |
→ **JIT 反而慢 45%**。栈提升没带来收益。

## 诊断（结论很重要，决定了下一步方向）
1. 瓶颈不是栈访问。把 `st[k]` 挪到 C 局部变量后一点没变。
2. 根因是 **TCC 基本不做优化**：它生成的机器码质量打不过 gcc -O3 编译出来的解释器循环
   （解释器每条 opcode 虽然有 dispatch 开销，但循环体本身是高度优化的 C）。
3. 所以"把 switch 分派换掉"这条路本身收益有限。**真正的收益只能来自去装箱/类型特化**：
   让内循环在 int 域里跑（`long long a,b,i` + 几条机器指令/迭代），
   而不是每条 opcode 都操作 8 字节 `JSValue` 做一些 tag 判断——
   这能把每次迭代从"数十条指令"压到"约十条"，量级上才可能赢过 -O3 的解释器。

## 下一步方案（类型特化 + guard + 去优化，就是用户提醒的弱类型风险点）
1. 生成器加一个**类型推断 pass**：判断某函数是否"全程 int 安全"（参数/局部/中间值都能在 int64 域
   表示，且不出现字符串/对象/浮点语义操作）。
2. 生成的 JIT 函数入口加 **guard**：实参不是 small int 就走通用路径（调用引擎的通用入口 / 回退解释器）。
3. 算术溢出、除法、比较等**边界**都要检查 → 一旦不满足就**去优化**（bail 回解释器，且不再尝试）。
4. 只有做了 1–3，才允许把 `a+b` 直接发成机器加法而不带任何 tag 判断。
5. 保底路线：TCC 适合"快启动 JIT"，峰值性能交给 **AOT（离线 gcc -O3 编译成目标文件再加载）**——
   这也正好对上 goal 里"空闲时把纯 JS 函数 AOT 成机器码"的定位。

# <10>: Step 7 —— 数值特化（int + double）已落地，JIT 从"慢一倍"追到"慢 12%"
UNRESOLVED（性能，但大幅改善）

## 设计（commit b6d8569）
- 生成器里加了**抽象解释** pass，类型格 `{UNKNOWN, NUM, BOOL}`；走字节码时同步做类型传播，
  跳转目标处比较"局部类型 + 栈类型 + sp"快照，不一致就放弃 JIT（回到纯解释器）。
- 全程数值的函数 → 整个函数在 **C `double` 域**里跑：局部变量和栈槽都变成 `double` C 局部变量。
  **为什么统一用 double**：JS 的 Number 本来就是 double，所以 `a+b` 在 double 域算就是规范行为，
  int32 溢出会自然变成非整数并被装箱成 float —— 于是**不需要任何溢出检查**，只有"根本不是数值"才处理。
- 装箱：数值返回时若落在 int32 且是整数值就 `JS_MKVAL(JS_TAG_INT, ...)`，否则 `JS_NewFloat64`；
  BOOL 类型则 `JS_NewBool`。
- 字面量：`push_const`/`push_const8` 里是 int 或 float64 的常量都支持（用 `%.17g` 打印 double 保精度）。
- **guard 而不是假设**（JS 弱类型）：读参数时判 `JS_TAG_INT` / `JS_TAG_FLOAT64`，其它类型置 `*deopt`；
  `JS_CallInternal` 收到 deopt 就清掉 `whale_jit_entry`、把栈帧恢复成当前帧，**用解释器重跑同一帧**。
  重跑安全，因为被编译的子集无副作用（无 call、无属性访问、不写 `var_buf`；有 `var_ref` 的函数直接拒绝）。

## 结果
| 版本 | fibi(30) |
|------|----------|
| 原版 QuickJS-ng 解释器（gcc -O3） | **1600 ns/call** |
| 未特化 JIT（通用 JSValue 域） | 3200 ns/call |
| **数值特化 JIT（本版）** | **1800 ns/call** |
正确性：400× `fibi(30)` 全对；浮点变体 `fsum(1000) = 249750` 300× 全对（double 路径确实被特化）。
→ 特化把差距从"慢 100%"压到"慢 12%"，但**还没赢过解释器**。

## 仍然没赢的根因
TCC 基本不做优化：生成的 double 运算反复 load/store（没有有效的寄存器分配），
每条语句都回到内存。我们生成的代码已经"算法级"更优（每次迭代约十条双精度运算 vs 解释器的
opcode 分派），但被代码质量吃掉了。

## 下一步（按收益排序）
1. **寄存器形态生成**：现在的生成仍是"栈机形态"（`v0/v1` 槽 + 跳转）。改成把字节码还原成
   结构化 C（`double a,b,i,n; while (i<n) {...}`，局部变量直接互算、无栈槽），TCC 至少能少几次内存往返。
2. **AOT 路线**：热点函数离线用 gcc -O3 编成 .o/.dll 再加载，峰值性能靠它；TCC 只管"快启动 JIT"。
3. **分支预测 / CPU 缓存优化**（用户提出，太复杂，先记着）：
   - 分支：生成代码里对"类型 guard 通过"这种大概率分支加 `__builtin_expect`/布局提示；
     循环回边对齐；把冷路径（deopt、异常）挪到函数尾部。
   - I-cache：控制内联/展开规模，别让膨胀伤指令缓存；TCC `tcc_relocate` 出来的代码页尽量连续
     （现在每个函数一个 TCCState，内存分散）；必要时按页对齐热函数。
   - D-cache：JSValue 数组/对象的布局与访问顺序（SoA vs AoS）、局部变量尽量留在寄存器、
     避免同一缓存行被多线程写。等有真实 profile 再做，别空优化。
4. TCC 选项（`-O1` 之类）试一下；编译缓存 + `free_function_bytecode` 里 `tcc_delete`（老账）。
5. 递归版 fib 仍走解释器（`call1` + `get_var_ref0` 不在支持面内）；支持 `call` 后才会覆盖 goal 要求的递归 fibonacci。

# <11>: Step 8 —— 表达式形态生成 + 参数解箱：JIT 反超解释器 9 倍
SOLVED

## 结果（fibi(30)，5 轮取最小）
| 实现 | ns/call |
|------|---------|
| 原版 QuickJS-ng 解释器（gcc -O3） | 1800 |
| JIT：栈机形态 + double 特化（<10> 的版本） | 1800 ~ 2000 |
| **JIT：表达式形态 + 参数解箱（本版）** | **200（9×）** |
正确性：400× `fibi(30)` 全对；300× 浮点变体 `fsum(1000)=249750` 全对。

## 关键改动（都在 `whale_jit_gen`，commit cd04d7f）
1. **两条 peephole 把"栈机形态"改成"表达式形态"**：
   - `<load> <load> <binop> [put_loc]` → 一条赋值 `L3 = L0 + L1;`
   - `<load> put_loc` → 直接拷贝 `L0 = L1;`
   于是 fibi 的内循环就是：
   ```c
   L6: v0 = (L2 < A0) ? 1 : 0;
       if (v0 == 0) goto L22;
       L3 = L0 + L1;  L0 = L1;  L1 = L3;  L2 = L2 + 1;
       goto L6;
   ```
   **为什么这一步是决定性的**：TCC 没有像样的寄存器分配、spill 凶（正是用户提示的那个 TCC 小毛病）。
   少一个活变量、少一次栈槽往返，就多一个值留在寄存器里。
   **常量折叠也从这条 peephole 里自然掉出来**（纯常量操作数在编译期就算完了）。
2. **参数入口一次性解箱**到 `A0..An`（仍有 guard：非数值 → deopt），不再每次 `get_arg` 重新解箱。
3. 修掉三个让生成器**静默放弃**的 bug：peephole 的栈效果算反（净 +1，我写成 -2/-1）、
   生成的函数**漏了结尾 `}`**（TCC 在 EOF 报 identifier expected）、"字节码正常走完"被当成失败。
4. 新增 `WHALE_JIT_DUMP=1` 把生成的 C 源码打到 stderr —— 上面三个 bug 全靠它找出来，调 JIT 先开它。

## 这印证了 <9>/<10> 的判断
不是 TCC 无药可救，而是**生成形态**问题：同一个循环，tcc 编"最优形态"要 135 ns/call、gcc -O3 是 15 ns/call，
都比解释器的 1800 好一个数量级。形态对了就赢回来。

## 下一步
1. **更多 opcode 层优化**（用户提的 AST/opcode 方向）：现在只有 peephole 级折叠；
   还没做 **不可达代码删除**（CFG 信息已有一半）、无用赋值消除、循环不变式外提、强度削减。
2. **支持 `call`**（`call1` + `get_var_ref0`）：递归版 fib 现在仍走解释器，goal 的 fibonacci 验证需要它。
3. **AOT 插入点（用户要求先留接口、不要依赖外部 gcc）**：把 `whale_jit_compile()` 抽成后端接口
   （`compile(src) -> entry`），当前实现是 TCC，将来可插 gcc/LLVM 做离线编译。
4. **分支预测 / CPU 缓存**：现在生成的代码分支很少，优先级降低（清单见 <10>），等有 profile 再做。
5. 老账：TCCState 泄漏、编译缓存、`qjs`/`whale_test` 共享静态库。

# <12>: Step 9 —— 自递归 fib 也能 JIT（call + get_var_ref）
SOLVED

## 支持面扩了两条
- **`get_var_refX`：只接受"闭包槽就是本函数自己"**的情况 —— 运行时用
  `whale_jit_is_self(whale_var_refs, whale_self, idx)` 校验。这条限制是关键：它保证被编译的函数是
  **纯函数**（自递归 + 数值运算），于是 deopt 之后重跑同一帧才是安全的（不会重复副作用）。
  检查已提升到函数入口（闭包在一个调用期间不变）。
- **`callN`：被调者必须是上面那种"就是自己"的槽** → 直接调用**自己的 JIT 入口**，不经 `JS_CallInternal`
  （自递归不需要新引擎帧：局部变量全是 `double` C 变量，闭包也是同一个）。
  第一版走了 `JS_CallInternal`，结果**一点没提速**（建帧开销把收益吃光了）。
- `var_refs` 以 `void *` 传给生成代码（免得生成的 C 需要内部类型 `JSVarRef`）。
- 新增 `whale_jit_stack_ok()`（`js_check_stack_overflow` + `JS_ThrowStackOverflow`），
  入口和每次自递归调用前都查 → 深递归抛 `RangeError`，不会把 C 栈压爆。

## 三个 bug（全靠 `WHALE_JIT_DUMP` 找出来）
1. 生成函数**丢了 `exc:` 标签**（重写时删掉了异常出口 emit）→ TCC: `label 'exc' used but not defined`。
2. 自递归调用的**函数指针强转少了新参数** → TCC: `too many arguments to function`。
3. **`is_self` 读错了数组**（最隐蔽）：解释器的 `get_var_ref` 用的是 `JS_CallInternal` 里的局部
   `var_refs`（= `p->u.func.var_refs`，闭包持有的引用数组），我读成了 `sf->var_refs`
   （帧上给**本函数内部闭包**预留的槽位）。后果：守卫必然失败 → 第一次之后 deopt、永远解释执行；
   表面"能跑、语义全对"，只是**没有提速**。教训：同一个函数里两个名字很像的数组，含义完全不同。

## 结果
| 用例 | 原版解释器 | WhaleJS JIT |
|------|-----------|-------------|
| 迭代 `fibi(30)` | 1800 ns/call | **200 ns/call（9×）** |
| 递归 `fib(24)` ×30 | 353 ms | **293 ms（1.2×）** |
语义全过：递归 fib、浮点递归、迭代 fibi、fsum、深递归 RangeError；`whale api test: ok`。commit c03e345。

## 递归只有 1.2× 的原因
每次自递归调用仍有：参数装箱 → 间接调用 → 返回值解箱 → 栈检查。要更快就得做**数值直传**：
给 JIT 函数再加一个"参数已解箱"的入口（`double whale_jit_num(JSContext*, double n, ...)`），
自递归直接用它，绕开装箱/解箱。

## 下一步
1. 数值直传入口（上面这条）。
2. opcode 层 **DCE / 循环不变式外提**（对应 `temp/tcc_to_optimize.md` 优先级 1、6）。
3. AOT 后端接口预留（`compile(src)->entry` 抽象，仍用 TCC，不硬依赖 gcc）。
4. `temp/tcc_to_optimize.md` 的 TCC 自身清单：语句边界强制刷寄存器（仅次于 spill）、无内联、
   无循环优化、常量传播弱、窥孔弱、无 CSE、分支/对齐缺失、栈槽不复用、别名分析缺失……
   其中"常量传播/DCE"和"窥孔"我们在生成器层就能吃；"跨语句寄存器保留"要动 TCC 的 vstack 管理（大工程）。

# <13>: Step 10 —— 常量传播 + 死代码消除（生成器层吃掉 TCC 清单前两项）
SOLVED

## 做了什么（commit 5182fd6）
1. **常量传播**：只被写一次的常量局部变量（`assign_cnt[var] == 1`）在读取处直接替换成字面量。
   `var k = 1.5; s = s + k*3` → 生成 `L0 = 1.5`，循环体变成 `v1 = 1.5 * 3`（TCC 再折成 4.5）。
   之前漏的一半：`1.5` 是 `push_const`（常量池），而 `whale_load_expr` 只认了 `push_iN` 系列 —— 补上后
   `push_const/push_const8` 里的 int/float64 也能参与折叠与传播。
   安全性：只在"该局部变量在整个函数里只被赋值一次"时传播，所以跨跳转/循环都成立。
2. **死代码消除**：`return`/`goto` 之后、直到下一个跳转目标之间的字节码不再生成。
3. **踩的坑（最危险的一类）**：`inc_loc`/`dec_loc` 也是写局部变量，但统计赋值次数时只数了 `put_loc*`
   → 循环计数器（`var i = 0` + `inc_loc`）被当成"只赋值一次的常量 0" → 循环条件恒真 → **JIT 代码死循环**。
   教训：传播类优化里"漏算一个写操作"在语义上完全说得通，只能靠测试兜住。
4. 另修一个工具坑：PowerShell 辅助函数用 `Write-Output` 返回状态，被 `$t = Rep ...` 捕获，
   写回时把 `B10 ok B9 ok ...` 拼到了 `quickjs.c` 开头（编译器立刻报错，没造成更坏后果）。

## 结果
| 用例 | 原版解释器 | WhaleJS JIT |
|------|-----------|-------------|
| 迭代 `fibi(30)` | 1800 ns/call | **200（9×）** |
| `cf(30)`（循环内含常量运算） | 1200 ns/call | **400（3×）** |
| 递归 `fib(24)` ×30 | 351 ms | **274 ms（1.28×）** |
语义：fibi / fib / fsum / cf / cf2 / 会重新赋值的变体 / 深递归 RangeError 全过 + `whale api test: ok`。

## 对照 temp/tcc_to_optimize.md
- 优先级 1（常量折叠 + 常量传播 + 简单 DCE）→ **本步在生成器层吃掉，不用改 TCC**。
- 优先级 2（窥孔：消冗余 mov、`lea` 替算术、跳转缩短）→ 属 TCC 内部，未做。
- 优先级 3（跨语句寄存器保留，仅次于 spill 的大头）→ 要动 TCC 的 vstack 管理，未做。
- 其余（无内联、无循环优化、CSE、分支/对齐、栈槽复用、别名分析）→ 未做。

## 下一步
1. 递归调用的**数值直传入口**（绕开装箱/解箱）—— 递归 fib 还有空间。
2. **循环不变式外提**（tcc_to_optimize.md 优先级 6）：我们已有跳转目标与回边信息。
3. **AOT 后端接口预留**（`compile(src)->entry` 抽象）。
4. TCC 内部优化（优先级 2/3），工作量大，按需再做。

# <14>: Step 11 —— JIT/AOT 后端接口（AOT 插入点）
SOLVED

- 把"C 源码 → 可调用入口"抽成结构体：
  `JSWhaleJitBackend { name, compile(ctx, src, &entry, &handle), release(handle) }`。
  当前唯一实现 `whale_backend_tcc`（`tcc_compile_string` + `tcc_relocate` + `tcc_get_symbol`，
  运行时 helper 符号也在这里注册）。
- `whale_jit_backend` 是**可替换的指针** —— 这就是 AOT 的插入点：将来用离线 gcc/clang + 磁盘缓存编热点函数，
  只要实现同一个结构体、改这个指针，生成器与 `JS_CallInternal` 挂接都不用动，
  也不会对"外部编译器"形成硬依赖（用户要求：先留接口，别硬依赖 gcc）。
- 行为不变：`whale_jit_compile()` 仍是"生成源码 → 交给后端 → 把 entry/handle 挂到 bytecode"。
  句柄释放仍是老账（代码里有 `ponytail:` 标注）。
- 验证（commit 75cd7b6）：build ok、`whale api test: ok`、fibi(30) 仍 200 ns/call（9×）。

# <15>: Step 12 —— 自查：JIT 为什么"没生效" + 数值直传（fib(40) 18.6s → 2.12s）
SOLVED

## 用户报的现象
他的 `fib.js`（`if (n <= 2) return 1; ... fib(40)`）用我们的 qjs 跑 17s，原版 16s，node 873ms。
自查结论：**三个独立原因叠加**。跟"编译开销"和"生成 C"都没关系（TCC 的接口就是 C，这条路实测 9×）。

### 原因 1：opcode 覆盖太窄
生成器只认 `lt/add/sub/mul`。他写的是 `n <= 2` → 字节码用 **`lte`** → 生成器直接放弃 → 全程解释执行。
修：`whale_binop_c()` 把数值二元运算映射成 C 运算符（`add sub mul div lt lte gt gte eq neq strict_eq strict_neq`），
外加 `mod / neg / plus / not / inc / dec` 与 32 位跳转 `if_false / if_true / goto`、`if_true8`。
（`not` 用 `(x != x) || (x == 0)`，这样 NaN 也是 falsy。）

### 原因 2（正确性 bug）：跳转目标处的栈深度
目标处的 `sp` 是**线性扫描**带过去的：`goto` 之后的死代码虽不生成，但扫过的字节码**没有回滚 sp**，
于是 `else` 分支把结果写进错误的槽 —— `h(30)` 返回 `-29` 而不是 `0`。
修：**在跳转指令处就用该边自己的 sp 登记目标状态**；进入目标时校验并**恢复快照**；
比较时只看两条边上都活跃的槽（全部局部变量 + 栈 `min(sp)`）。

### 原因 3（环境致命）：JIT 后端用了相对路径
`WHALE_JIT_SRC_DIR "src"` / `"thirdparty/tcc-runtime"`。从 `build/mingw/x86_64/release` 跑 `qjs fib.js` 时 cwd 变了
→ TCC 报 `include file 'quickjs.h' not found` → **静默回退解释器**（正是用户看到的现象）。
修：xmake 在配置期注入**绝对路径**宏。坑：Windows 路径的反斜杠必须换成 `/`，
否则 C 字符串字面量会把 `\a` 之类当转义吃掉（`'\a'` 是 BEL）。
另外 `WHALE_JIT_DUMP=1` 时会打印实际使用的两个路径，方便排查。

### 原定计划第 1 项：数值直传
生成两个入口：
- `whale_jit_core(ctx, args, double *An, int *deopt)` —— 全程 double，零装箱；
- `whale_jit_fn(...)` —— 引擎调用的 JSValue 包装（入口解箱一次、出口装箱一次）。
自递归**直接调 core** → 递归不再"装箱参数 → 重入引擎 → 解箱结果"。
`is_self` 检查移到包装层（一次调用里闭包不会变，递归无需重复校验）。
顺带：TCC **不能在生成代码里按值传递聚合类型**（`JSValue` 在某配置下是 union），所以 core 只收 `ctx/args/An/deopt`。

## 结果
| 用例 | 原版 | WhaleJS JIT |
|------|------|-------------|
| **用户 fib.js（fib(40)，从 release 目录）** | 17.5 s | **2.12 s（8.3×）** |
| 递归 fib(24) ×20 | 154 ms | **19 ms（8.1×）** |
| 迭代 fibi(30) | 2000 ns/call | **200 ns/call（9×）** |
（node 跑同一个 fib(40) 是 873 ms，还差 2.4×；剩下的是 TCC 代码质量 + 我们不是真优化器。）

## 教训
1. **"没提速"先确认到底有没有走 JIT**：`WHALE_JIT_DUMP=1` 一次就能区分"生成器放弃 / 编译失败 / 没被调用"。
2. 生成器静默放弃（`goto out`）这种设计很危险：语义永远对、性能悄悄归零 —— 必须有诊断出口。
3. 相对路径写死在二进制里，会让同一个 exe 在不同 cwd 下行为不同。

# <16>: Step 13 —— 改用 TCC 的 IR 是更优路线（附带干掉 .h 依赖）+ libtcc 两种链接方式
TIPS（方向）+ SOLVED（构建）

## 用户确认的方向：直接生成 TCC 的 opcode/AST，别经过 C 源码
TCC 已停更、可以随意改，所以这条路可行。**额外好处（本轮确认）**：
- **不再需要 include `quickjs.h`** → `<12>`/`<15>` 里那个"`.h` 找不到"的路径问题从根上消失
  （问题本质是 TCC 要按 C 编译器的方式解析头文件；直接发 IR 就没有这个环节）。
  注意 `libtcc1-64.a` 等运行期文件仍然需要路径，只是头文件依赖没了。
- 跳过 C 解析/类型检查 → JIT 编译更快（现在单函数 ms 级，函数多了差距会放大）。
- 我们直接持有 IR：寄存器保留、窥孔、循环优化都能在 IR 上做，
  不必再靠"生成更友好的 C 源码"去猜 TCC 会怎么发码。
- 落点：`tccgen.c` 的 `vpushi` / `gen_op` / `g()` 与 `SValue`/vstack。
  建议第一步做"把一个数值基本块直接发成 TCC op"的最小闭环，接通后再扩展。

## libtcc 现在两种链接方式（commit ec7287f）
- **默认**：从 `thirdparty/tinycc/libtcc.c` 编成静态库（我们要魔改 TCC，所以默认走这条）。
- **可选**：`xmake f -y -p mingw -m release --libtcc_prebuilt=y` → 链接预编译 `libtcc.dll`。
  导入库 `liblibtcc.a` 用 `dlltool -d libtcc.def -l liblibtcc.a -D libtcc.dll` 生成后入库
  （`thirdparty/tcc-runtime/lib/`，与 `libtcc1-64.a` 放一起）；构建只 relink（0.8s），
  构建后把 dll 拷到二进制旁边。这条也满足 goal 里"优先预编译版本直接链接"。
- `.gitignore` 加了 `!thirdparty/tcc-runtime/lib/*.dll`。
- 验证：两种模式都 `fib(32)=2178309`（48ms / 51ms），源码模式 `whale api test: ok`。

## 用户实测
修完 `<=` 覆盖 + 跳转状态 + 路径之后，用户自己跑 fib(40)：**1949 ms**（我这边 2.12s，量级一致），
相对原版 17.5s 约 9 倍。

# <17>: Step 14 —— 把 JIT 生成器拆成独立编译单元（src/whale_jit.c）
SOLVED

## 做了什么（commit 41fe765）
- 生成器（~950 行）从 `quickjs.c` 搬到 `src/whale_jit.c`；引擎与它之间只通过结构体通信：
  `JSWhaleFuncInfo { byte_code, byte_code_len, is_normal, arg/var/stack 计数, var_ref_count, cpool }`，
  生成器**不碰 quickjs 内部结构**。
- 生成器需要的三个内部服务由引擎以薄封装提供：
  - `JS_WhaleVarRefIsSelf(ctx, var_refs, idx, self)` —— 证明"就是自己"（只编译直接自递归）
  - `JS_WhaleCheckStack(ctx)` —— 栈溢出检查（同时抛 RangeError）
  - `JS_WhaleOpSize(op)` —— **操作数长度直接问解释器**（`short_opcode_info`），不再自己维护表
- 收益：**只改 JIT 生成器 → 增量重建 0.5 秒**（此前每改一处都要重编 6.5 万行的 quickjs.c，约 60 秒）。

## 两个坑（都值得记住）
1. **TinyCC 必须保持独立编译单元**。TCC 的 `tcctok.h` 和 quickjs 的 token 表**都用 `DEF()` 展开枚举**
   （`TOK_VOID`/`TOK_IF`…）→ 把 `libtcc.c` include 进 `quickjs.c` 必然重定义。
   要从生成器里用 TCC 内部 API，正确做法是**在 TCC 那一侧加导出薄封装**（libtcc API 本身就是这个套路），
   而不是合并 TU。
2. **自己生成 opcode size 表踩坑**：`quickjs-opcode.h` 第一次 include 就已经把 `DEF` 与 `def` 两类项都展开了
   （`def` 默认落到 `DEF`），我 include 了两次 → 表翻倍、DEF 区之后的 size 全部错位 →
   生成器在第一条 opcode 就"静默放弃"（诊断先是 `pos=0 op=215`，改对映射后暴露真凶 `get_var_ref0`）。
   现在 size 一律由引擎提供，不可能再漂移。
   （顺带确认过：两边枚举是一致的 —— `get_var_ref0=227 add=156 count=252 temp_start=185 temp_end=204`。）

## 验证
- `bad=0`（pristine 与 whalejs 在 h/fib/fibi 上完全一致）、`whale api test: ok`
- 用户 fib.js：**2.22 s**（原版 17.5 s）
- 只 touch `src/whale_jit.c`：`build ok, spent 0.5s`















# <18>: Step 15 —— TinyCC 内部接口骨架（为"直接发 IR"铺路）
SOLVED（骨架与链路）/ 待办（真正的 IR 通路）

## 做了什么
- 新增 `src/tcc/whale_tcc_iface.c`，由 `libtcc.c` **末尾 include** → 它编译在 TinyCC 自己的 TU 里，
  因此能直接摸 TCC 内部（`tcc_state`、`vstack`/`vtop`，将来 `vpushi`/`gen_op`/`g`）。
  这就是 <17> 那条结论的正解：**不合并 TU，而是在 TCC 一侧加导出封装**。
- 探针 `JS_WhaleTccProbe()` 返回 `VSTACK_SIZE`（来自 tcc.h，256）→ 链路通且值可验证；
  `WHALE_JIT_DUMP=1` 时 JIT 打印 `whale tcc internal probe: vstack cap=256`。
- 生成器侧（`src/whale_jit.c`）在 `libtcc.h` 之后声明即可调用。

## 改动点地图（优先级 2/3 与 IR 直发的落点）
| 位置 | 行 | 作用 |
|---|---|---|
| `x86_64-gen.c` | 146 / 158 / 166 / 326 | `g()` 逐字节发码 / `o()` 发 32 位 / `gen_le16` / `gen_modrm` |
| `tccgen.c` | 2154 / 3069 / 663 | `gen_op()`（op 归一化核心）/ `vstore()` / `vswap()` |
| `tcc.h` | 424 / 1283 / 1305 | `SValue` / `vstack`+`vtop` 全局（容量 256）/ `check_vstack()` |

- **发 IR** = 把生成器的 op 序列直接喂给 `vstack` + `gen_op`，跳过 `tccpp`/`tccgen` 的解析；
  额外好处是**不再 include quickjs.h**（<16> 已提），`.h` 路径问题从根上消失。
- **做窥孔** = 得在 `g()`/`o()` 外面加一层缓冲：TCC 现在是单遍流式发码，做跨语句优化前
  必须先缓存整个函数的 `gen_op` 序列（temp/tcc_to_optimize.md 的架构分叉点）。

## 验证
- `whale api test: ok`；pristine 与 whalejs 在 fib/fibi 上 `bad=0`；用户 fib.js 2.2~2.6s（抖动区间内）。
- 探针 256 == tcc.h 的 `VSTACK_SIZE`。
# <19>: Step 16 —— IR 直出：框架就位，首次调用崩（待修）+ 优先级 2/3 与扩大受益面规划
PARTIAL / PLANNED

## 已做
`src/tcc/whale_tcc_iface.c` 增加 `JS_WhaleIrCompileConst2(a,b,op)`：**不生成任何 C 源码**，
完全照 `gen_function`(tccgen.c:7054) 的帧序列手工走 ——
`sym_push2` 建返回类型 Sym + 函数 Sym（`CType.t = VT_FUNC`、`.ref` = 返回类型 Sym、参数链为空）
→ `cur_text_section = text_section` → `put_extern_sym` → `gfunc_prolog` →
`vsetc(&double, VT_CONST, …)` ×2 → `gen_op('+')` → `gv(RC_FLOAT)` → `gfunc_return(&func_vt)` →
`gfunc_epilog` → `tcc_relocate` + `tcc_get_symbol`。

两个发现值得留：① TCC 的**单字符运算符直接用字符值**（`gen_op('+')`，不是 `TOK_PLUS`，tccgen.c 里到处是 `gen_op('*')`/`gen_op('&')`）；
② `TOK_*` 枚举已由 `tcc.h:1070` 的 `enum tcc_token` 提供，**不要再造一份**（我造了一次 → `TOK_INT` 重定义）。

**状态**：能编过，但第一次实际调用**崩**（`WHALE_IR_PROBE=1` 时连 `fib(30)` 都打不出来）→ 已改成环境变量开关，
默认关闭；正常路径不受影响（`fib(30)=832040 13ms`、`whale api test: ok`）。

## 崩溃排查方向（下一步，按可疑度排序）
1. **段还没建**：`tcc_new()` 只建 state，`text_section` 等是 `tcc_add_file` 时才建的 →
   可能要先显式 `tccelf_new(s)`（或走 `tcc_add_file` 后再补 `cur_text_section`）。
2. **全局解析状态不完整**：`file`、`loc`、`func_ind`、`local_scope`、`nocode_wanted`、`funcname` 的期望值
   （`gen_function` 是在解析上下文里被调的，我们是裸调）。
3. `sym_push2(&local_stack, …)` 在函数体外是否合法。
4. `gv(RC_FLOAT)` / `gfunc_return` 对 double 的值栈形态期望。
做法：在 iface 里逐句打点（或 gdb），先分出是「建帧」「发码」还是「relocate」阶段崩。

## 优先级 2/3 规划（TCC 侧，依 temp/tcc_to_optimize.md）
- **优先级 2 · 窥孔（低风险，预期 +5~15%）**：落点 `x86_64-gen.c` 的 `g()`146 / `o()`158 / `gen_le16`166 / `gen_modrm`326。
  在 `g()` 外挂一层**小缓冲**（记最近若干指令的字节与 `ind`），识别：冗余 `mov rA,rB; mov rB,rA`、
  用 `lea` 取代 `add`/小常数乘、`gsym_addr` 跳转缩短。因为 TCC 是**单遍流式发码**，
  缓冲必须建在 `ind` 层（`cur_text_section->data + offset` 可以回写）。
- **优先级 3 · 跨语句寄存器保留（中高风险，预期 +20~50%）**：落点 `tccgen.c` 的 `gen_op`2154 / `gv`1253 / `vstore`3069。
  让 vstack 上的值尽量留在寄存器（现在 `load`/`save_regs` 太保守）；彻底版是缓存整个函数的 op 流再做寄存器分配。
  **这条只有 IR 直出通了才动得了** —— 否则 TCC 从 C 源码出发，发码形态我们插不上手。

## 扩大受益面（比峰值分数重要）
现状：只有「纯 double 计算 + 自递归」的函数会 JIT，其它一律回退解释器。要扩三块：
1. **整数域去守卫**：现在所有数值统一当 double（`for` 计数器也走 double）。
   加 `WHALE_T_INT` 类型格，整型发 `int64`/`lea`/`imul`；超出 int32 范围时转 double 以保 JS 语义。
   **诚实的预期**：x86-64 上 double 与 int64 的算术吞吐接近，收益主要在**比较、取模、数组下标**，不会翻倍。
2. **混合函数（数字 + 属性访问 + 字符串）**：现在是「遇到一条不认识的 opcode 就整体放弃」。
   (a) **分块 JIT**：按可编译性切基本块，可编的编、其余 call 解释器；
   (b) **慢路径内联**：生成代码里留 call 到解释器 helper（第一版 JIT 用过，后来为了「保证纯函数」删了）。
   推荐 (a)，风险在**精确的栈/寄存器状态映射**（进出解释器时必须严丝合缝）。
3. **外部调用**（`Math.*`、`console.log`、C 扩展经 `tcc_add_symbol` 注册）：
   认 `get_field` + `call` 形态，能静态认出 callee 就发**直接调用**，否则发**通用 call helper**；
   收益点是调用点不必回解释器建帧。
- **共同前提**：类型格要能表达「未知但已守卫」（现在只有 UNKNOWN/NUM/BOOL/SELF），
  且 deopt 要能**从任意点安全重入**（当前只保证「整帧重跑」）。

## 指令集与架构（用户提到的）
- **x86-64**：TCC 64 位已走 SSE 标量（不用 x87），可在此基础上加 —— `lea` 做整数乘加、`cmov` 消分支、
  必要时手写 **AVX2**（TCC 可改，`x86_64-gen.c` 里 `o(...)` 直接发字节即可）。
- **SIMD 的真正用武之地是数组/TypedArray 循环**（`for(i) s += a[i]*b[i]`），需要先认循环形态再发向量指令 —— 记为方向。
- **arm/arm64**：暂无测试环境，**保持兼容即可**：生成器与架构无关（只认 ops），
  架构相关部分全在 TCC（`arm64-gen.c`/`arm-gen.c` 已在 repo 里），换 `TCC_TARGET_ARM64` 就能编，本轮不动。

## 验证
- 正常路径：`fib(30)=832040` 13ms、`whale api test: ok`。
- IR 直出：`WHALE_IR_PROBE=1` 可复现崩溃（已知，未修）。
# <20>: Step 17 —— IR 直出：两个架构结论 + 卡在访问违例
PARTIAL

## 结论 1（重要）：TinyCC 的编译**不能并存**
`tcc_new()` 的语义是"初始化 TCC 的全局环境"，**不是**"创建可并存的实例" ——
`tcc_state`、`sections`、`text_section`、`table_ident`、`sym_free_first`、`ind`/`loc`/`func_vt`
全是**进程级全局**。实证：
- 第一版探针放在 JIT 的 TCC 编译路径**内部** → 堆损坏（`0xC0000374`）；
- 移到"无活跃编译"的时点、并把 `tcc_state`/`cur_text_section`/`local_stack`/`global_stack` 全都 save/restore
  → 仍崩（`0xC0000005`）。说明**只要同一进程里先后有两次 TCC 生命周期，残留全局就会互踩**。
→ **设计必须"一次编译一个版本"**：IR 通路与 C 源通路**不能并存**，用开关二选一。
   这与"按参数类型签名分版本"的模板化思路天然同构（见下）。

## 结论 2：IR 直出确实**不需要 include 路径**（<16> 的预测得到验证）
中途报过 `library 'libtcc1-64.a' not found`（我漏了 `tcc_set_lib_path` + `tcc_add_library_path`），
补齐即消失。也就是说 IR 通路只要**库路径**，**不需要** `quickjs.h` 的 include 路径 ——
"`.h` 路径问题从根上消失"成立。

## 现状
`JS_WhaleIrCompileConst2(tcc_dir, a, b, op)` 已写完全套（建帧 → 发码 → relocate），能编过；
在 `whale_test` 最前面（进程还没有任何 TCC 状态时）调用 → **立刻访问违例**，连 printf 都出不来。
已从测试里摘除，回归保持绿：`whale api test: ok`、`fib(30)=832040 21ms`。

## 下一步（定位方案）
在 iface 里逐句 `fprintf` 打点，先分出崩在哪一段：
① `tcc_new`/路径设置 ② `sym_push2` 建符号 ③ `gfunc_prolog` ④ `gen_op`/`gv` ⑤ `gfunc_epilog`/回填 ⑥ `tcc_relocate`。
怀疑集中在 ②③：`sym_push2(&local_stack, …)` 依赖 `local_stack` 已由 `tccgen_init` 建好，
而裸调可能走在解析器预期之外的状态里。

## 用户指导（纳入设计）：动态类型要按**参数签名分版本**
动态类型是 JIT 的大坑，**尤其传参**。方向：像 C++ template 实例化那样**按参数类型签名分版本** ——
同一个 JS 函数遇到 `(double,double)` 编一版、`(int,int)` 编一版、`(string,*)` 回退解释器；
调用点按实参类型分派到对应版本（配合守卫 + 单态内联缓存）。
这与结论 1 天然吻合（一次编译 = 一个版本），应作为"扩大受益面"里**类型格与调用约定的基础设计**，
也直接服务于"混合函数"和"外部调用"两块。
# <21>: Step 18 —— IR 直出：崩点精确定位到 `gv(RC_FLOAT)`
PARTIAL（定位完成，就差最后一步）

## 采纳用户诊断：**不建第二个实例**
用户指出"tcc 的初始化会动全局变量，第二次初始化就会混乱两个的内容；应该做侵入性修改，
使**一个实例可以完成多个任务**，而不是建立两个实例"（并指出 TCC 没有实例下属的 ctx）。
这完全正确，也修正了我 <20> 的表述：不是"不能并存"这种宿命，而是**不该新建实例**。
本轮的改法：`JS_WhaleIrCompileConst2` 不再用 `tcc_compile_string` 当初始化（那会 `preprocess_end`
把 token 环境拆掉，之后的 `tok_alloc` 往已释放内存写 → 堆损坏），改为照抄 `tcc_compile()`(libtcc.c:623) 的序列：

```
tcc_open_bf → tccelf_begin_file → preprocess_start → [我们发 IR] → preprocess_end
→ sym_pop(global/local) → tccelf_end_file → tcc_close → tcc_relocate
```

## 打点结果（`WHALE_IR_DEBUG=1`）
```
ir: tcc_new  →  ir: paths  →  ir: unit opened  →  ir: tok_alloc  →  ir: symbols
→  ir: prolog  →  ir: body start  →  ir: push a  →  ir: push b  →  ir: gen_op  →  ✗ 崩
```
**结论**：`gfunc_prolog`（建帧）✓、`vsetc` 压两个 double 常量 ✓、`gen_op('+')` ✓ 全部通过，
**崩在 `gv(RC_FLOAT)`**（把 vtop 的值取到寄存器）—— 即"取值"这一步，不是建帧也不是降级。

## 下一步（很窄了）
看 `tccgen.c` 的 `gv()`(1253) 在 `RC_FLOAT` 路径上依赖什么我们没设的状态。候选：
- `cur_func_sym` / `func_vt` / `func_var` 之外的"当前函数"全局；
- `loc`/栈帧已分配但 **`gv` 需要的 `vtop->type.ref`**（double 的 `ref` 我设成 NULL 了，可能期望
  `double_type` 那样的规范类型）；
- `tcc_state->nosse` 等浮点策略标志；
- `gv` 内部走 `load()` → `get_reg()`，而 `get_reg` 依赖"函数已经开始发码"的隐含状态。
建议：在 `gv` 入口与 `load`/`get_reg` 里各加一行打点，几轮内能收敛。

## 状态
IR 调用已从 `whale_test` 摘除，回归保持绿：`whale api test: ok`、`fib(30)=832040`。
打点保留在 iface 里（默认静默，`WHALE_IR_DEBUG=1` 才打印）。
# <22>: Step 19 —— IR 直出的机器码已经正确生成，卡在"入口地址"这一步
PARTIAL（只差入口地址）

## 这一轮的实质进展：**IR 通路全线打通，直到生成出正确机器码**
`WHALE_IR_DEBUG=1` 的分步打点全部通过：
```
tcc_new → paths → unit opened → canonical types → tok_alloc → symbols
→ prolog → body start → push a → push b → gen_op → (tcc 内部 gv 正常: r=0x30=VT_CONST)
→ gfunc_return → epilog → unit closing → unit closed → relocated
```
并且 dump 出的机器码**结构完全正确**（`double f(void){ return 2.5+4.0; }`）：
```
55                     push rbp
48 89 e5               mov rsp, rbp
48 81 ec 00 00 00 00   sub rsp, 0
f3 0f 7e 05 00 00 00 00  movq xmm0, [rip+0]   ← 常量折叠成一个 double，装进 xmm0
c9                     leave
c3                     ret
```
（`[rip+0]` 的偏移由重定位填，relocate 之前 dump 看到 0 是正常的。）

## 关键修复（三个真 bug）
1. **`vtop`/`nocode_wanted` 等解析状态**：不能拿 `tcc_compile_string` 当初始化（它末尾 `preprocess_end`
   会拆掉 token 环境 → 之后的 `tok_alloc` 写已释放内存 = 堆损坏）。改为照 `tcc_compile()`(libtcc.c:623)
   的序列开/关翻译单元。
2. **漏了 `tccgen_compile()`(tccgen.c:235) 开头的全局规范类型**：`int_type`、`size_type`、`ptrdiff_type`、
   `func_old_type`（还有 `anon_sym`/`section_sym`/`const_wanted`/`nocode_wanted`）。`gen_op`/`gv` 内部要用
   它们 —— 缺了就直接在 `gv` 里访问违例。补上后 `gv` 正常（`r=VT_CONST`）。
   这是本轮最有价值的发现：**"照抄帧序列"不够，还要照抄解析器的全局类型初始化**。
3. **不要自己 `gv(RC_FLOAT)`**：`gfunc_return(&func_vt)` 自己会取值且知道正确的寄存器类。
   我先前手写的 `gv(RC_FLOAT)` 是多余的（也曾是崩点）。

## 现在唯一的卡点：relocate 之后取不到可调用的入口
- `text_section->data + func_ind`：relocate 后 `text_section->data` **不可读**（一读就访问违例），
  说明它不再指向那块可执行内存（可执行但不可读，或已被回收）。
- `tcc_get_symbol(s, "whale_ir_fn")`：返回的地址比段基址高 1272 字节（`0x4f8`），
  但调用它**同样**访问违例。
→ 所以要么入口地址取法不对，要么常量段的重定位没落到可执行内存里。
**下一步**：查 TCC 在 Windows/`TCC_OUTPUT_MEMORY` 下 `tcc_relocate()` 之后正确的入口获取方式
（看 `x86_64-link.c`/`tcc_run` 怎么用 `tcc_get_symbol`），以及 `tccelf_end_file` 之后
常量段（rodata）是否也被搬进可执行映像。**不要再 dump `section->data`，那里已经不可读。**

## 状态
IR 调用已从 `whale_test` 摘除（避免崩），打点与字节 dump 保留在 iface 里（`WHALE_IR_DEBUG=1`）。
回归保持绿：`whale api test: ok`、`fib(30)=832040`。
# <23>: Step 20 —— IR 直出：机器码**完全正确**，但还不能安全调用（PE 相关）
PARTIAL（机器码正确，调用崩）

## 铁证：生成的机器码是对的
`double whale_ir_add(double a, double b) { return a + b; }` 经 IR 直出得到的字节：
```
55                     push rbp
48 89 e5               mov rsp, rbp
48 81 ec 00 00 00 00   sub rsp, 0
66 0f d6 45 10         movq [rbp+16], xmm0     ← 存参数 a（PE/x64：参数在正偏移）
66 0f d6 4d 18         movq [rbp+24], xmm1     ← 存参数 b
f3 0f 7e 45 10         movq xmm0, [rbp+16]     ← 读 a
f2 0f 58 45 18         addsd xmm0, [rbp+24]    ← xmm0 += b
c9                     leave
c3                     ret
```
参数符号也验证正确：`a(r=0x132 c=16)`、`b(r=0x132 c=24)`（`VT_LOCAL|VT_LVAL` + PE 正偏移）。

## 本轮踩通的关键点（都是"没照抄解析器就不行"）
1. **PE 与 ELF 是两套代码生成器**！`x86_64-gen.c` 里 `gfunc_prolog`/`gfunc_epilog` 各有两份：
   **PE 版在 953/1021，ELF 版在 1435/1587**（后者在 `#ifndef PE` 里）。我们编 `TCC_TARGET_PE`，用的是 PE 版。
   - PE 版参数：`addr = PTR_SIZE*2` 起递增，`sym_push(sym->v & ~SYM_FIELD, type, VT_LOCAL|VT_LVAL, addr)`
     → **参数在正偏移 `[rbp+16]`、`[rbp+24]`**，`loc` 保持 0（我先前以为 loc 该是负数，是照 ELF 版想当然）。
   - PE 版 epilog 自己回填 prolog（`ind = func_sub_sp_offset - FUNC_PROLOG_SIZE`）并调 `pe_add_unwind_data`。
2. **`sym_push2` 不复制 `CType.ref`**（只有 `sym_push` 才 `s->type.ref = type->ref`）。
   所以构造函数类型必须自己设 `funcsym->type.ref`。
3. **`gfunc_prolog` 里参数是"新建"局部符号**，和我们手里的参数节点无关 —— 要用
   `sym_find(tok)` 才能拿到它分配的偏移。这一条是参数访问的正确姿势。
4. **必须装 `setjmp(s->error_jmp_buf)`**：`tcc_error()` 是 longjmp，没有跳转目标时任何报错都是直接崩。
5. **不能调 `sym_pop(&local_stack)`**：它按解析器的形状释放符号链，我们的栈形状不同，会踩内存。
6. **构建坑（重要）**：`whale_tcc_iface.c` 原来是"被 libtcc.c include"的，xmake 看不见这种依赖 ——
   改它不会重编 libtcc。现在反过来：**`whale_tcc_iface.c` 是编译单元，它 `#include "libtcc.c"`**，
   xmake 的 `add_files` 指向它，两个文件都被跟踪。

## 现在唯一剩下的卡点
机器码正确、入口地址正确（`tcc_get_symbol` 与段基址一致）、`VirtualProtect(PAGE_EXECUTE_READWRITE)`
也加了 —— 调用仍然访问违例。因此剩下最可疑的是 **PE 侧的函数表/展开信息**：
`pe_add_unwind_data` 写入的 `.pdata` 是否被正确 link，以及 `win64_add_function_table`
（tccrun.c:305，只有 `tcc_run` 路径用 `RtlAddFunctionTable` 注册）在我们这条路径上没被调用。
**下一步**：在 `tccrun.c` 找 `VirtualProtect`(284) 那段所在函数，看它是 `tcc_relocate_ex` 还是别的；
确认 `_WIN64` 下我们是否需要显式注册函数表；再对比 `whale_tcc_compile`（C 源码路径，可正常调用+8×）
在 relocate 之后比我们多做了什么 —— 这是最直接的对照实验。

## 状态
IR 调用已从 `whale_test` 摘除，回归保持绿：`whale api test: ok`、`fib(30)=832040`。
`JS_WhaleIrCompileAdd2(tcc_dir)` 保留完整实现与分步打点（`WHALE_IR_DEBUG=1`）。
# <24>: Step 21 —— IR 直出：机器码正确，调用仍崩；穷举后的剩余嫌疑
PARTIAL

## 本轮又排查掉的（都已补齐，仍崩）
逐行对照 `gen_function`(tccgen.c:7054) 后补上的：
- `tcc_debug_funcstart(tcc_state, sym)` / `tcc_debug_funcend(tcc_state, size)`（PE 下参与函数边界/展开信息）
- `gsym(rsym)`、`label_pop(&global_label_stack, NULL, 0)`
- 收尾完整照抄 `tcc_compile()`(libtcc.c:623)：`preprocess_end` → **`free_inline_functions`** →
  **`free_defines(define_start)`** → `sym_pop(&global_stack)` → `sym_pop(&local_stack)` → `tccelf_end_file` → `tcc_close`
  （`free_defines` 这一步先前漏了，导致 `sym_pop` 踩内存）
- `VirtualProtect(PAGE_EXECUTE_READWRITE)` 显式加在入口页上（排除 DEP）
- `sym_find(tok)` 取参数偏移（已验证 `a(r=0x132 c=16)`、`b(r=0x132 c=24)` 正确）

## 现在的事实
- **机器码完全正确**（反汇编验证，见 <23>）。
- **入口地址正确**：`tcc_get_symbol` 与 `text_section->sh_addr` 一致。
- **可执行权限已加**。
- **调用仍然访问违例**（0xC0000005）。

## 下一步：做一次真正的双路径对照（这是收敛最快的路）
不要再逐条猜。在 `src/whale_jit.c` 里做一个临时对照：
1. 用现有 C 源路径（`tcc_compile_string`）编译 **同一个签名** 的函数
   `double whale_jit_fn(double a, double b) { return a + b; }`，relocate 后调用 —— 它**一定成功**。
2. dump 它的函数字节（在 relocate **之前**读 `text_section->data`），与 IR 版逐字节比对。
3. 差异点就是答案。重点看：
   - 函数前后是否有额外数据（`.pdata` 展开记录、对齐填充、符号表项）；
   - `pe_add_unwind_data` 写的东西在 C 路径和 IR 路径是否一致；
   - `whale_tcc_compile` 在 relocate 后是否还有我们没做的调用（目前看没有，它只 `tcc_get_symbol` 然后 `tcc_delete`）。
4. 如果字节一致而行为不同，那就是 **state 层面的差异**（临时文件、`s->runtime_mem`、函数表注册）。

## 状态
IR 调用已从 `whale_test` 摘除，回归保持绿：`whale api test: ok`、`fib(30)=832040`（无性能损失）。
`JS_WhaleIrCompileAdd2(tcc_dir)` 保留完整实现 + 分步打点（`WHALE_IR_DEBUG=1`）。
# <25>: Step 22 —— 双路径对照：工具层面踩了两个坑，对照本身还没做成
PARTIAL

## 这轮想做的事
在 `src/whale_jit.c` 里 dump **C 源路径**生成的函数字节（它一定能跑、8×），与 IR 版逐字节比对。
只要字节有差异，差异点就是答案；若字节一致，问题就在 state 层。

## 踩到的两个坑
1. **`whale_jit.c` 看不到 TinyCC 的内部类型**：它只 include `libtcc.h`（公开头），
   所以写 `Section *ts = s->sections[k]` 直接编译失败（`unknown type name 'Section'`、
   `invalid use of incomplete typedef 'TCCState'`）。dump 这类活儿必须由
   `whale_tcc_iface.c` 提供（那个文件才在 TinyCC 的 TU 里）—— 我为此在 iface 里加了
   `JS_WhaleIrDumpText(TCCState *s, const char *tag)`。
2. **但在 JIT 路径里调这个 dump 会把正常的 JIT 弄崩**（`qjs` 跑 fib 直接 exit 1、
   没有任何输出）。原因未查清（很可能是 dump 触碰了 `s->sections` 里尚未就绪的段，
   或者与 JIT 自身的编译顺序冲突）。**已把它从 JIT 路径移除**，回归恢复。

## 另一条被证伪的尝试
在 iface 里加 `JS_WhaleIrAdd2C()`（用 `tcc_compile_string` 编**同一签名同名字**的函数，
与 IR 版做对照）—— 它**在 printf 之前就崩**，比 IR 版还早就死。
说明"另一个 state 里的 C 源编译"在这条调用路径上也不成立，所以对照必须在
`whale_tcc_compile` 那条**已被证明可用**的路径里做（而不是另起一个函数）。

## 结论：对照实验的正确做法（下次直接照做）
不要新写对照函数，**改 `whale_tcc_compile` 自己**：
1. 在它 `tcc_compile_string` **之后**、`tcc_relocate` **之前**，
   调 `JS_WhaleIrDumpText(s, "c-source")` —— 但这次要先确认第 2 个坑的原因
   （先只在 dump 里打印 `s->nb_sections` 和每段的 `sh_type/sh_flags/data_offset`，不读 `data`，
   看是哪一步崩）。
2. 拿到 C 源路径的字节后，与 IR 版（`WHALE_IR_DEBUG=1` 已能打印）逐字节比对，
   重点看函数开头/结尾的填充、`.pdata` 展开记录、以及函数之间是否有额外符号表项。
3. 若字节一致 → 问题在 state 层（临时文件、`s->runtime_mem`、`win64_add_function_table` 注册）。

## 状态
回归绿、无性能损失：`whale api test: ok`、`fib(30)=832040 14ms`。
IR 直出仍是"机器码正确但调用崩"。`JS_WhaleIrDumpText` 留在 iface 里备用。
# <26>: Step 23 —— 外部文件依赖已证明**可以消掉**（这次真的看到了）；方向定为 AST 优先
PARTIAL（可行性已验证，实现被打断）

## 用户指出的结构性矛盾（这是关键判断）
C 编译路径的致命问题不是性能，而是**外部文件依赖**：生成的 C 代码引用 `JSValue`，就必须
`#include "quickjs.h"` —— **这使 WhaleJS 不能成为一个干净的库**（要带着头文件才能 JIT）。
用户给的方案：**不要手工模仿 TCC 的流程**，而是 **hook / 手写简易协程覆盖原本 codegen 的输入**
—— 直接从源头输入，**优先直接输入 AST，退而求其次才是 token 流**；并明确"TCC 不需要更新，
做侵入性更改完全没有问题"。

## 本轮实测：依赖确实可以去掉（重要）
外部依赖的全部来源就是 `src/whale_jit.c:172` 的 preamble 第一行：
```c
static const char *whale_jit_preamble =
    "#include \"quickjs.h\"\n"      ← 就这一行
    ...
```
把它换成内联声明后：
```c
"typedef unsigned long long JSValue;\n"      /* 64 位下 JSValue 就是 8 字节按值传递，ABI 一致 */
"typedef struct JSContext JSContext;\n"      /* 不透明指针 */
"#define JS_EXCEPTION ((JSValue)6 << 32)\n"  /* JS_TAG_EXCEPTION=6, JS_MKVAL 在 64 位是 tag<<32 */
```
并**删掉 `tcc_add_include_path(s, WHALE_JIT_SRC_DIR)`** —— 结果 **JIT 在没有设置任何 include 路径的
情况下跑通了**：`fib(32)=2178309`。也就是说 **<16> 里"`.h` 路径问题从根上消失"不再是推测，
是实测过的**。唯一缺的是生成代码用到的 quickjs 常量/原型，需要逐个内联。

## 本轮为什么回退了
补常量时我一次性塞进 `JS_UNDEFINED`/`JS_NULL` 两个 `#define`，结果 `fibi(30)` 算错
（`v == 832040.0` 断言失败）。原因未查明（很可能是宏与生成代码里的同名标识符冲突）。
已 `git checkout` 回退，**回归恢复绿**。下次要**一次只内联一个符号**，并单独跑
`fib`/`fibi` 两个用例验证。

## 下一步（按用户的优先级）
1. **AST 直出优先**：TCC 的 codegen（`gen_op`/`gv`/`gfunc_*`）本来就是"AST 消费者"，我们构造
   表达式直接喂它 —— 我一直在做的就是这个，卡点是**解析期上下文不完整**。
   要继续的话，重点排查 `tccgen_compile()`(tccgen.c:235) 与 `gen_function()`(7054) 之外、
   由 `block()`/`stmt()` 建立的那些状态（`define_stack`/`label`/`local_scope` 等）。
2. **退路 token 流**：hook `tccpp` 的 `next()` 或直接驱动 `tccgen` 的 token 接口，
   让 TCC 自己走完整流程（状态永远正确），我们只提供 token —— 这条路对"状态正确性"最稳。
3. 顺带：**把 preamble 的依赖彻底消掉**（按上面"一次一个符号"的办法补完），
   这一步和 1/2 独立，可以随时做，收益是整个库不再需要外部头文件。

## 状态
回归绿、无性能损失：`whale api test: ok`、`fib(30)=832040 15ms`。
本轮未提交的改动已回退，工作区干净。IR 直出仍停在"机器码正确、调用崩"（<24>/<25>）。
# <27>: Step 24 —— **突破**：断开 lexer、接自定义 token 流 → IR 直出通了
SOLVED（通路验证成功）

## 用户的方案是对的，实测通过
用户指出：**不要手工模仿 TCC 的流程**，而是「**把 token generator 断开，连接自定义的流**」，
让 TCC 自己的解析器跑；并指出上下文不该成为问题 —— 除了 QuickJS 那几个定义（可以在初始化时
编译一遍，甚至转换成结构体定义免去解析），**其余一切应该当作空文件**。

## 实现（三处，都很小）
1. **hook 点**：`src/tcc/tccpp.c:2990` 的 `next_nomacro()` —— 它是"取一个 token、设 `tok`/`tokc`"的
   核心，`next()`(3529) 只是它外面包了宏展开。在它开头加：
   ```c
   if (whale_tok_active) { whale_next_token(); return; }
   ```
2. **token 源**（`src/tcc/whale_tcc_iface.c`，与 TCC 同一个 TU）：
   - `whale_tok_buf[64]` + 长度/游标；`whale_next_token()` 设 `tok`、`tokc`、`tok_flags`；
   - 标识符用 **`tok_alloc(name, len)->tok`**（必须走这个才能进 token 表）；
   - 关键字直接用 `tcctok.h` 的枚举（`TOK_DOUBLE`/`TOK_RETURN` 等），标点用自己的字符值。
3. **驱动**：`tcc_open_bf` → `tccelf_begin_file` → `preprocess_start` → 构造 token → 开 `whale_tok_active`
   → **`tccgen_compile(s)`** → 关 → 收尾 → `tcc_relocate` → `tcc_get_symbol`。

## 结果（本仓库实测）
```
[tok] token-stream compile: compiled
      result = 6.50 (expected 6.50)
whale api test: ok
```
即：**用 token 流编译出的 `double whale_ir_add(double a, double b){ return a + b; }` 能被调用并返回正确结果。**

## 为什么这条路是对的（对比我前面十几轮的弯路）
- **上下文问题彻底消失**：`decl()`/`block()`/`gen_function()`/`gen_op()` 全是 TCC 自己跑的，
  状态由构造保证正确 —— 不再需要手工照抄 `tccgen_compile()` 的初始化、`gen_function()` 的序列，
  也就不会再遇到"机器码正确但调用崩"那种状态错位。
- **外部文件依赖彻底消失**：token 里只有内建类型（`double`），**没有任何 `#include`** ——
  `quickjs.h` 的头文件路径问题、以及"生成 C 源再词法分析"这一整层都没了。
- 这同时验证了 <26> 的判断：WhaleJS 可以做成**不依赖外部头文件**的库。

## 下一步
1. **把 JIT 生成器从"拼 C 源码"改成"发 token"**：`whale_jit_gen` 现在往字符串里写 C 文本
   （`whale_emit`），改成往 `whale_tok_buf` 里写 token；类型落到内建类型上（double/int64），
   外部函数用 `tcc_add_symbol` 注册 + 自声明原型。**这样整个 JIT 不再需要 C 文本和 include。**
2. 之后才是循环展开、SIMD（用户提的后续），SIMD 可以在 token 层发内建类型 + 我们注册的
   向量 helper，或者改 `x86_64-gen.c` 直接发指令。

## 状态
回归绿、无性能损失：`whale api test: ok`、`fib(30)=832040`。
（`check_ir_direct` 目前只在测试里跑 token 流这一条路径，不影响原有 JIT。）
# <28>: Step 25 —— token 通路定位到根因：**标识符撞上了 TCC 的 asm 关键字**（未修完，已回退）
PARTIAL

## 根因（本轮唯一的真正产出，但很值）
把生成的 C 源码 dump 出来（`WHALE_JIT_DUMP=1`）后发现：**用 `gcc -fsyntax-only` 验证它完全合法**。
所以问题不在源码，而在**我的词法器把某个标识符 token 化成了关键字**。全量 dump token 后一眼看到：

```
... [294]double [42]* [702]out [41]) ...
```

别的参数名（`ctx`/`var_refs`/`idx`/`self`/`d`/`v`/`An`/`deopt`）都拿到了新标识符 id（1269、1270…），
**只有 `out` 拿到 702** —— 那是 `tcctok.h` 里 **asm 汇编器的 `out` 指令（`TOK_OUT`）**。
于是 `double *out` 的参数名被吞成关键字，TCC 看到 `double * )` 就报 **`function pointer expected`**。

**教训**：`whale_keywords` 表是从 `#include "tcctok.h"` 生成的，它包含的**不只是 C 关键字**，
还有 asm 助记符（`out`/`in`/`push`/`pop`…）等。**生成代码里的标识符必须避开整张表**，
否则会被静默当成关键字。修法二选一：给表加白名单（只收真正的 C 关键字），或让生成器避开这些名字。

## 本轮做过的改动（**已全部回退**，文件被我改坏且预算耗尽）
hook `next_nomacro()`(tccpp.c:2990)、token 源、`whale_lex_fragment`、`JS_WhaleCompileTokens`、
preamble 自包含（无 `#include`、无 `#define`、`<<`/`>>` 进运算符表、`JS_EXCEPTION` 用 `tcc_add_symbol` 注册）、
`whale_jit.c` 改走 token 通路并去掉 include 路径。
**其中已验证成立的**：去掉任何 include 路径后最小 token 通路仍能跑（`result = 6.50`）—— 这条在 f5e9667 里。

## 下一步（按此顺序，别再摸黑）
1. 给 `whale_keywords` 加白名单，或让生成器避开 `TOK_OUT` 这类撞名。
2. 重做上述改动（描述见 <26>/<27>），**先只做 `out` 改名**并验证。
3. **把 `gcc -fsyntax-only temp/gen.c` 固化成常规自检**：生成的 C 先让 gcc 验一遍，再喂 token 流。
   这次正是它一句话把方向从"TCC 嫌语法"扭到了"我的词法出错"。

## 状态
工作区已回到 **f5e9667**：`whale api test: ok`、`fib(32)=2178309 44ms`（8× 仍在），无未跟踪文件。