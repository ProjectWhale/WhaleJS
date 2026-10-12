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
# <29>: Step 26 —— 用户点破：**"生成 C 再扫回 token"是自找麻烦**（已回退）
PARTIAL / 方向修正

## 用户的判断（正确）
"你能生成 C 代码不就能自己生成 token 流吗" —— 我却一直走"生成 C 文本 → 复刻一个 C 词法器把它扫回 token"，
而**麻烦全部来自复刻 TCC 的词法**：关键字表、字面量解析、多字符运算符。`out` 撞 asm 关键字（<28>）
就是这个方案带来的 bug，而不是问题本身。

## 正确的做法（下次直接这么做）
`whale_emit(fmt, ...)` 那 55 个调用点**直接发射 token**，不要经过文本：

```c
whale_emit("    double v%d = 0;\n", i);
   ↓
wtok(TOK_DOUBLE); wtok_id("v"); wtok_int(i); wtok('='); wtok_int(0); wtok(';');
```

这样：
- **不需要词法器**（不扫文本、不需要关键字表、不需要字面量解析）；
- 关键字映射缩减到"emit 里真正出现的那几个"（`double`/`int`/`void`/`static`/`extern`/`return`/`if`/`goto`…），
  且**只可能撞我知道的名字**，不会有 `out` 这类意外；
- 本文的 `whale_tok_buf` / `whale_tok_push` / `whale_tok_active` / `next_nomacro` hook（都在 f5e9667 里）**可以原样复用** ——
  它们是这次唯一站得住的部分。

## 本轮试过并再次失败的
按上面的思路做了"白名单 + 词法器 + JS_WhaleCompileTokens"，但：
- 白名单里 `TOK_SIGNED`/`TOK_CONST`/`TOK_VOLATILE`/`TOK_INLINE` 在 TCC 里其实叫 `TOK_SIGNED2`/`TOK_CONST2`/…；
- 大段文本替换在 PowerShell 里反复把文件搞坏（几次都是替换文本插错位置）。
**这两条本身就是"这条路太绕"的证据。**

## 状态
工作区回到 **f5e9667**：`whale api test: ok`、`fib(32)=2178309 44ms`（8×），无未跟踪文件。
下一步：改 `whale_emit` 的调用点为直接发 token（小步、可验证、不再写词法器）。
# <30>: Step 27 —— 直接发 token：两端接上了，但 token 流中途停（已回退到绿）
PARTIAL

## 这几轮的两大发现（都是真知识）
1. **根因一（关键）**：`whale_tcc_compile` 里**一直还在调 `tcc_compile_string(s, src)`** ——
   生成器早已改成发 token，编译器却还在读那份**残缺的 C 文本**。两头根本没接上，
   这就是"折腾这么多轮始终不通"的答案。
2. **根因二**：**发 token 必须在 TU 打开之后**。`tok_alloc` 需要 token 表，而
   `whale_jit_gen` 原先在 `tcc_new`/`preprocess_start` **之前**被调用 → 直接访问违例。
   现在改成 `JS_WhaleCompileTokens(s, emit_cb, ctx)`，回调在 `preprocess_start` 之后执行。

## 现在的卡点（未解决）
`dump` 重建出来的 token 文本**只有 952 字节**，停在 `... { double v0 = `；
而 `whale_jit_gen` 并没有报 `gen gave up`（返回 1），所以是**发 token 中途断了**，
不是生成器拒绝编译。TCC 报 `declaration expected`。这两个现象必须一起解释：
**下一步就从"为什么 token 只发到 `v0 = `"入手**（在 `wt_*` 里加计数打点，看是哪一个 helper
或哪一处 `goto out` 提前结束）。

## 定位手法（这次真正奏效的，值得固化）
`JS_WhaleTokDump()`（iface 里）把 token 流用 `get_tok_str()` 重建为文本，直接和预期的 C 对照。
- `TOK_INT = 256 = TOK_IDENT`，所以 `int` 在 dump 里会渲染成空 —— 那是 dump 的表现，不是缺失；
- 一旦看到**序列在某处戛然而止**，就说明生成端停了，而不是 TCC 挑剔。

## 完成度
- `whale_load_expr` → `whale_load_analyze` + `whale_load_emit`（peephole 不能试探性地发 token）
- `whale_binop_c` → `whale_binop_op`（返回 `JS_OP_*`）；新增 `JS_WhaleTokKw/Ident2/Slot/Int/Double/Char/Op`
- `whale_jit_gen` 里**所有**真实 emit 点已改成直接发 token（`whale_emit` 只剩定义 + 一处死代码）
- 关键字/运算符**用我们自己的 id**（`JS_WK_*` / `JS_OP_*`），TCC 侧映射到 `TOK_*` —— 生成器永不写 `TOK_*`，也就不会再撞 `out` 那种关键字

**代码已回退到 f5e9667**（绿：`whale api test: ok`、`fib(32)=2178309 38ms`）；
上述工作保留在 git 历史 `5716d97`→`0bb5b25` 里，需要时 `git checkout 0bb5b25 -- src` 取回。
# <31>: Step 28 —— 用"读代码"而非"跑测试"找到的三个真 bug（重要教训）
TIPS

## 用户的关键反馈
"你太依赖测试了，我一行代码都没看就能指出你那么多错误，跳出来看看呢" —— 正确。
本轮**只读 emit 代码**（不试探性地跑），一次找出 3 个真 bug；其中第一个解释了我之前所有"没头绪"的观察：

1. **`wt` 与 `wk` 混用（37 处）**：`wt(x)` 是**标点/字符**发射器（发 `x` 这个字符值），
   `wk(w)` 才是**关键字**发射器（发 `TOK_*`）。我把 `wk(JS_WK_EXTERN)` 等 37 处写成了 `wt(JS_WK_*)`，
   于是 `extern`/`int`/`if`/`goto`/`return` 全被发成了**控制字符 token**（如 `JS_WK_EXTERN`=8 → `\b`）。
   —— 这同时解释了 dump 里"`extern`/`int` 渲染为空"：它们根本不是关键字 token。
2. **`void` 走错发射器**：`wt_id("void")` 应为 `wk(JS_WK_VOID)`（关键字不能当标识符发）。
3. **`JS_WhaleVarRefIsSelf` 原型返回类型写错**：写成了 `JSContext *`，应为 `int`。

修完这 3 处后，TCC 的报错**从 `declaration expected` 变成 `function pointer expected`** ——
说明 token 流大幅接近正确，但还剩至少一处（`function pointer expected` 的语义是：
`decl()` 里遇到"不是类型名的标识符 + `(`"，即某个**参数列表开头不是类型**）。

## 教训（写给下一次）
- **先读、再推**：emit 点是我自己写的，正确性靠**逐行和 C 语义对照**就能验，不必每次都构建+运行。
  我此前十几轮的"改一处→构建→跑→没头绪"正是反例。
- 对照清单（读代码时逐项核）：**关键字必须走 `wk`**、**标识符才走 `wt_id`**、
  **类型名与关键字不能混**、**每个 extern 原型的返回类型**。
- `JS_WhaleTokDump()` 是辅助，**不是判据**：`TOK_INT = 256 = TOK_IDENT`，`int` 渲染为空是正常表现。

## 状态
回归坏于此轮末尾（`whale_test` 撞访问违例），**代码已回退到 f5e9667**（绿：`whale api test: ok`、
`fib(32)=2178309`，8×）；本轮工作保留在 git `1bdeaae`（及 `5716d97`..`0bb5b25`）。
# <32>: Step 29 —— 又一次自我纠正：**观测工具的故障被当成了被观察对象的故障**
TIPS

## 我错在哪
上一个 <31> 里我断定"token 流只发到 `{ double v0 = ` 就断了"。**错了。**
真相：`JS_WhaleTokDump()` 自己会崩 —— 它用 `get_tok_str(t, NULL)` 打印每个 token，
而 `get_tok_str` 对 `TOK_CINT`/`TOK_CDOUBLE` 会按"标识符"去索引 token 表（越界）。
dump 恰好停在 `v0 = ` 之后的**第一个数字字面量**（`wt_int(0)`）就是证据。

**所以：dump 断了 ≠ token 流断了。** 我把自写工具的输出当成了判据，而那个工具本身有 bug。

## 用什么方法纠正的（值得复用）
**把 emit 代码逐行"手译"回等价的 C，再用 `gcc -fsyntax-only` 检查**：
```
temp/read_back.c:26:5: error: label 'L20' used but not defined   ← 唯一的错是我手写时编的标签
```
即：**我读出的序列语法完全正确**（`static double whale_jit_core(JSContext*, JSValue*, double*, int*) {...}`
与 `JSValue whale_jit_fn(...) {...}` 全部合法）。gcc 接受、TCC 却报 `function pointer expected`
→ 差别**不在 token 的合法性**上。

## 结论与下一步
- token 序列**语法正确**；TCC 仍报 `function pointer expected`（含义：`decl()` 遇到"非类型名的标识符 + `(`"）。
- 问题方向已从"我的 token 发错了"转为 **"TCC 没把我们的类型名认出来"**：
  查 `decl()` 里判断标识符是否为类型名走的是哪个函数（`sym_find` 一类），
  以及我们经 `tok_alloc` 得到的 token 在它看来是否可见。**这仍是一次纯读代码的排查，不需要跑 WhaleJS。**

## 教训（与 <31> 合并成一条）
1. **先读代码**（rules-for-LLMs.md 里本来就有这条，我反复忘）；
2. **不要依赖自写观测工具的输出**：先确认工具本身正确（`get_tok_str` 不能用于 CINT/CDOUBLE）；
3. 观察与推理冲突时，**先怀疑观察**。
# <33>: Step 30 —— token 通路的修复进展 + **通用优化方向清单**（用户要求）
PARTIAL + PLAN

## 本轮修好的（都是"读代码"找出来的，不是猜）
1. **`tcc_open_bf(s, tag, 0)`** → 改成 `1` 并把缓冲显式清空、`buf_ptr` 归位。
   —— 这是 `whale_test` 访问违例的根因（长度为 0 时 parser 初始化没有可用缓冲）。
   *注：我一度以为长度 0 一定崩是"buffer 没分配"，读 `tcc_open_bf` 后知道它用 `IO_BUF_SIZE` 且 `mallocz`
   清零 —— 真实原因更微妙，但改成显式清空后崩溃确实消失。*
2. **`whale_tok_active` 的守卫提到 `tcc_new()` 之前** —— `tcc_set_output_type()` 内部会调
   `tcc_define_symbol()`，而后者**直接调 `next_nomacro()`**（绕过 `tcc_compile`），
   一旦开关残留为 1，它就会读我们已耗尽的 token 流。
3. **`JS_WhaleTokCompileAdd2()` 补上 `setjmp` 保护** —— 它原先没有，一旦内部 `longjmp`
   就把开关永久留成 1，污染后续所有 `tcc_define_symbol`。
4. **翻译单元命名去掉尖括号**：`<whale-jit>` → `whale_jit.c`（尖括号是 TCC 给内建伪文件的习惯，
   会漏进 `__BASE_FILE__` 的定义）。
5. token 流本身**已验证正确**：文本（dump）与数值（`553` 个 token，`299/267/279/1265/265/256/1267…`）双证。

## 仍未解决
`parse_define()` 报 `invalid macro name '='`：宏名位置读到的不是标识符。
打点显示最后一个 `tcc_define_symbol` 是 `__BASE_FILE__`，其值为**带引号的文件名**。
**下一步**：查 `__BASE_FILE__` 的定义处，以及 `next_nomacro()` 读到引号后 `parse_define()` 的期望。

## ===== 通用优化方向清单（对所有代码受益，不只是纯数值函数）=====

### A. AST / IR 类（前端，改写程序结构）
- **A1 循环展开（unroll）**：按 trip count 或小常数展开，减少分支与循环开销。
  这是"让所有代码受益"里性价比最高的之一；对 `for` 计数器类代码直接有效。
- **A2 循环不变量外提（LICM）**：把循环内不随迭代变化的表达式提出来。
  TCC 完全不做，收益在循环体里做属性访问/重复计算时最明显。
- **A3 归纳变量强度削弱**：把 `i*k+c` 形式的地址/索引运算改成递增的加法。
- **A4 内联小函数**：不只是自递归。短小的被调函数内联后能连带消除参数装箱。
- **A5 标量替换 + 逃逸分析**：本函数内新建的对象/数组若不逃逸，拆成局部标量。
- **A6 数组边界与长度缓存**：循环里 `arr.length` 这类重复读，提到循环外。
- **A7 常量折叠/传播/DCE**：已完成（<13>），继续覆盖更多 opcode 形态。

### B. 后处理 / 指令选择类（后端，不改程序结构）
- **B1 窥孔优化**：消冗余 `mov`、`lea` 替 `add`/小常数乘、跳转缩短。
  落点 `x86_64-gen.c` 的 `g()`/`o()`。**低风险、对所有代码受益**。
- **B2 跨语句寄存器保留**：TCC 在每个语句边界 spill；让值尽量留在寄存器里。
  中高风险，但**收益面最广**（任何代码都吃这个）。
- **B3 SIMD/向量化**：数组/TypedArray 循环（`s += a[i]*b[i]`）是最典型的受益者；
  也可以用于 `Math.*` 批量运算。需要先认循环形态，再发向量指令。
- **B4 现代指令选择**：`cmov` 消分支、`imul`/`lea` 组合、AVX2（x86-64 基线之上）。
- **B5 栈槽复用 / 小函数内联后处理**：降低栈帧与访存。
- **B6 分支布局/对齐**：把热路径排在一起，改善 I-cache（已记入 notes，未做）。

### C. 运行期架构类（让更多 JS 形态可编译，**受众最广**）
- **C1 整数域（去类型守卫）**：`for` 计数器、数组下标走 int64，超 32 位再回 double。
- **C2 混合函数分块 JIT**：按基本块可编译性切分，能编的编、其余回调查解释器。
  —— 这一条能覆盖"真实代码"（数字+属性+字符串混用），是**摆脱'跑分战神'的关键**。
- **C3 外部调用直通**：`Math.*`、`console.log`、C 扩展识别为直接调用，不回解释器建帧。
- **C4 值类型表示分层**：现在只有 UNKNOWN/NUM/BOOL/SELF；补 STRING/OBJECT/INT。
- **C5 deopt 从任意点重入**（现在只支持"整帧重跑"）—— C2/C3 的前提。

### 排序建议（性价比 × 受益面）
1. **B1 窥孔**（低风险、立竿见影、所有代码受益）
2. **A1 循环展开**（AST 类里最直接，且不依赖后端改动）
3. **C1 整数域**（让循环类真实代码受益）
4. **B2 寄存器保留**（收益最大但风险高）
5. **C2 分块 JIT**（受众最广，但依赖 C5）
6. **B3 SIMD**（特定场景爆发力强）
# <55>: 混合编译（单指令 fallback）实施与止步：loop buffer 越界写 + fallback 接线引入内存破坏

UNRESOLVED

已按 notes/mixed-compilation-boundary.md 的设计把"通用 fallback"端到端做出来（引擎侧 `JS_WhaleOpArity`/`JS_WhaleStepCovers`/`JS_WhaleStepRun` 与 Push/Out/Clear 一组，生成器侧 `whale_step_emit` 接进 `default:` 和 5 个 case 的拒绝点）。覆盖确实变宽了：24 个 perftest case 的 `gen gave up` 从 **23 降到 10**，剩下 10 处指向引擎尚未执行的指令（`call2`/`call_method`/`return`/`add_loc`/`get_field2`）。

但它踩出两件真问题：

1. **loop buffer 越界写 + 静默丢代码（已修）**。`WHALE_LOOP_BUF`=4096，而 `fold_commit()` 与 `loop_end()` 都用 `*whale_out_len < WHALE_TOK_MAX`(32768) 作追加上限 —— 循环体打开时 `whale_out_tok` 指向 `WhaleLoop.tok[4096]`，于是超过 4096 token 的循环体**既越界写数组**，尾部又被 `loop_end()` 的 `min(l->len, WHALE_LOOP_BUF)` **无声丢掉**。该缓冲区从未被任何 loop 级优化使用（它是死重量）。修法是让 `loop_begin` 直接返回 0（走它本来就有的"缓冲满则直通"路径），`whale_loop_depth` 恒为 0，`loop_end` 永不进入。一个函数，无新机制；24 case 结果不变。

2. **`JS_NewArray` 在 JIT 帧内返回 `{tag=-1, ptr=有效堆指针}`（= `JS_TAG_UNINITIALIZED`）**。`JS_IsException()` 不拦它，于是这个值被停进对象槽、又被当作对象交给 `JS_SetPropertyValue` → `objshape`/`nested2d` 运行期 0xC0000005。`JS_NewArray` 本身只有三行（`JS_NewObjectFromShape(ctx, js_dup_shape(ctx->array_shape), JS_CLASS_ARRAY, NULL)`），而同一进程里解释器调用它上百次都正常。

   **已排除**（都是实测，不是推理）：`cur_sp` 借用（改成 NULL 结果完全一样）、对象槽机制本身（`object`/`objfields`/`array` 都正确且未变）、<46> 的帧尺寸顺序问题（`whale_jit_ready` 仍然在管）、`whale_frame_obj_slots`/`JS_WhaleObjSlot`/`JS_WhaleArrNew`（读写都正确）、以及"fallback 没被 emit"（`[emit] called op=72 pos=66 sp=3` 已确认执行）。

   **⚠️ 结论修正**：上面那条 `JS_NewArray` 的"异常"是**症状，不是根因**。在 `JS_WhaleArrNew` 里加**一行无关的** `JS_NewObject(ctx)`，崩溃与错误 tag **一起消失**（`A check=66`，与 node 一致），去掉又回来 —— 这是内存破坏的指纹。另外 TinyCC 在接线状态下对三行复现报 `duplicate label 'L14'`，而**同一复现在基线上是干净的**（`h check=5`，与 node 一致）。所以要查的是 **`whale_step_emit` 周围的 emit 流与生成期状态**（它在 case 体里调 `fold_materialize()` 与 `fold_mark()/fold_commit()`，那里可能已经有 label 被 emit 过），不是引擎 helper。

工作树已回退到最后绿色提交（`loop_begin` 的修复保留）；完整工作 diff 存于 `temp/whale_step_work.patch`（UTF-16 原始）与 `temp/whale_step_work_u8.patch`（转成 UTF-8，`git apply --check` 可解析；对 `86e5a52` 应用时只有两处已知冲突：`JS_WhaleArrNew` 的旧诊断块、`loop_begin` 的修复前版本）。详见 notes/whale-step-investigation.md。

# <56>: five JIT fixes, and the slot identity that still blocks immutable_update
TIPS

## what changed (all committed, tree green, 24/24 correct)

- `c114f05` a function that returns a call result can compile. OP_return accepted only
  OBJ/ARR slots; a general call result is WHALE_T_VAL, so `var v = f(); return v;` threw
  the whole function away. e2/e1: 72ms -> 16ms.
- `5353954` a laid-out object used as a call argument is materialised into a real object
  first (JS_WhaleObjNew + JS_WhaleObjDefineNum per column).
- `3d426e9` one materialisation slot per layout, not one per site, so two sites cannot
  meet at a jump target as different types.
- `361a96b` whale_out_line: every `goto out` records `__LINE__` and the "gave up" line
  prints `bail=N`. Also WHALE_OBJ_SLOTS 12->28, WHALE_CALL_BASE 8->20.
- `6156b80` a self-call with a non-numeric argument takes the general route, which
  learned to call the function's own entry (JS_WhaleCallSelfValue + `whale_self`).

## the diagnosis that worked

Stock qjs on the same case. An interpreted whalejs time sits almost exactly on the
interpreter's - that is how e2/e1 were caught (72 vs 74, 71 vs 77, while compiled
functions were 60x faster), and it reads a "not compiled" list off a full run:

    immutable_update 550 vs qjs 573    <- interpreted
    vnode_build      347 vs qjs 361    <- interpreted

`bail=N` then names the exact line. Guessing from pos/op is worse: the position
reported is usually a jump target, not the place that refused.

## the blocker left in immutable_update

It has two readings; one now compiles. The other stops at bail=2441, the jump-target
type merge (whale_jit.c:2409-2441): one slot carrying two different KNOWN types on two
edges into the same target drops the whole function.

Both types are "a JSValue in a slot" - one from materialising a layout, one from a
call's return value - but a slot's identity in this generator IS its slot number, so
they compare unequal. I32/ICONST already merge above that test; OBJ/VAL do not.

Fixing it needs an object type that does not name a slot, or a slot-number-to-variable
map at the merge. Architecture, not a one-liner. That is where this stopped.

## ruled out - do not re-tread

- object-slot exhaustion: `[mat-arg] objs=1 callbase=20`, plenty free.
- materialisation not running: it runs, `struct=1`, and the result is passed on.
- code volume: fourfield and w_of2 dumps are line-for-line the same, binaries 2798 vs
  2494 bytes, 685 vs 613 instructions - yet 65ms vs 3ms. Still unexplained. Not code
  size, not the loop body, not the map path (JS_WhaleObjFieldPtr appears 0 times).

## <57> native control-flow compilation (for/if/while/switch) - preparation

PLAN only, no code change this round. Full note: `notes/native-control-flow-plan.md`.

**What the measurement changed.** `objfields` and the same loop body with the fields
replaced by plain locals (`temp/w_of_split.js`) run at the same speed:

    whalejs  OF 16/32/57/72ms   LOC 15/35/51/61ms   (~16 ns/iter)
    node     OF  4/10/15/16ms   LOC  3/7/10/22ms    (~4  ns/iter)

So object field access is NOT the bottleneck - the loop body as a whole is ~4x node.
`WHALE_STR_STAT` also showed `StructNew` / `FieldNum` / `SetNum` / `col_lookup` are
all called **zero** times on this case: no CStruct is built and no table is consulted.
Both layout tables are complete (`cols=4 atoms=[tag,key,flags,depth]`, `mono=1`).

**An older reading is withdrawn.** The previous note's "D plain locals 3ms vs C object
fields 70ms, 23x" compared two *different* loop bodies; the gap was the modulo, not the
object. Same body, same speed.

**Modulo guards are not the target.** `fold_mod` already lowers to integer `idiv`, and
the project's own measurement at `whale_jit.c:4519-4523` says
`guarded idiv 1776ms, bare idiv 1781ms, fmod 3028ms / 2e8 ops - the guard is free`.
Trimming them buys nothing; not done.

**Where the change goes.** QuickJS has no `OP_for`/`OP_while` - for/while/do are
`goto` + `if_false` in the bytecode, and the generator translates them 1:1 into C
(`OP_if_*` at 4677, `OP_goto*` at 4700, with a `snap`/`seen` type snapshot per jump
target, taken exactly once). "Native for" means recognising the loop shape in that
goto graph and emitting a C `for` instead.

**Predicted bugs, in order of likelihood** (detail in the plan note): the one-shot type
snapshot is wrong if a slot's type changes across iterations; `deopt` inside a loop
re-runs the frame and repeats side effects; `sp` must be identical at head and back
edge; `unreachable`/DCE interacts with the back edge; nested `break`/`continue` need
the C `break` scoping; labels may still be referenced from outside the loop.
## <58> compiler-side loop table, attached to JSFunctionBytecode - done

The generator no longer has to infer a loop from the opcode stream. QuickJS has no
OP_for/OP_while, but its stack-size pass already computes every jump target, so a
backward jump (`tgt < pos`) can be recorded there - deterministic, not inferred.

Implemented (all in `src/quickjs.c`, no generator change yet):

- `WhaleLoop { head, back, exit, kind }` + `WHALE_LOOP_MAX 64` (near WhaleProfSite).
- `JSFunctionDef` carries the collected table.
- `compute_stack_size`: `whale_note_loop(fd, pos, tgt, cond)` added to the `goto`/
  `goto16`/`goto8` and `if_true*`/`if_false*` branches (~39021-39044).
- `js_create_function`: memcpy into `JSFunctionBytecode` (inline array, so it stays out
  of the GC and the clone path and needs no free).
- `JSFunctionBytecode`: `uint16_t whale_loop_count; WhaleLoop whale_loops[64];`
- Diagnostic `WHALE_LOOP_DBG=1`.

Measured: `objfields` `blen=102 loops=1 {head=5 back=66 cond=0}`; `object`
`{head=22 back=74}`; `sieve` 4 loops including a nested one; every case correct,
`0 mismatches`, timing identical to baseline (pure collection).

`cond` distinguishes a backward `if` (do/while test) from a backward `goto`
(for(;;)/while). `exit` is still -1: it is the target the head's conditional jump
leaves through, i.e. what `break` uses; second pass.
## <59> try/catch native: works, but not yet a win

Three bugs, each masking the next, all found by dumping the generated C:

1. `return JS_WhaleExceptionValue(ctx)` was emitted inside the core, and the core
   returns a **double**. TinyCC's "invalid aggregate type for register load" is
   exactly that. The `*deopt == 2` path already means "exception pending, do not
   re-run", so the value is unused: return 0.
2. `OP_catch` parked undefined in a stack slot but left the fold state saying the slot
   was not materialised, so a later op inlined an empty expression there.
3. **The real one.** `OP_nip_catch` only did `sp--`. Its shape is
   `catch; <value>; nip_catch`, and it removes the exception from *under* the value -
   so the value has to move down into the slot the exception held. Missing that move,
   `return x*2` inside the try read the catch block's undefined. A small probe looked
   correct; a 1e6-iteration loop returned 0 for 999934 of them.

Verified against node on four probes, and 0 mismatches over the 24 perftest cases.

**Not a win yet**: 225ms compiled against the same function interpreted at 156ms and
node at 5ms. Per call, on the normal path, the generated code does
`JS_WhaleObjSlotUndef` (the catch slot), `JS_WhaleGetGlobalObj` (`Error`),
`JS_WhaleObjSlotAtom` (the message) and `JS_WhaleConstructValue` - four helper calls
for a path where nothing is thrown 99.99% of the time. Direction, in order of value,
is in `notes/exception-optimisation.md`: initialise the catch slot at the handler
instead of at `catch`; make the throw's preparation lazy; and the outstanding one -
**a hand-written C baseline compiled at -O0/-O1** per perftest case, without which
"already at the native ceiling" and "the codegen is doing extra work" cannot be told
apart.
# <60> immutable_update: why a reducer never compiles, and six layers down

`immutable_update.js` is the Redux shape - `setField(o, v) { return { a: o.a, ...,
e: v }; }` and a loop that reassigns the result. It reported 549ms, and `entries`
(committed this session) showed **no entry at all**: it was never compiled, so that
number was the interpreter. Chasing it down found six layers, each hiding the next.
The work is reverted - see the end for why - but the layers are the value here.

## The mechanism that was missing

`whale_ret_obj_g` (scan pass, `OP_return`) decides whether the objects a function
builds are **real JSObjects** (it returns one, so it leaves and another compilation
meets it) or **frame-column structs** (it keeps them, needs no allocation). The test
was:

    if (prev_op >= OP_get_loc0 && prev_op <= OP_get_loc3 &&
        whale_loc_obj[prev_op - OP_get_loc0])
        whale_ret_obj_g = 1;

That only recognises `return v` where v is a local holding a built object.
`setField` ends with `return { a: o.a, ... }`, whose opcode before `return` is
`define_field`, so it was **not** recognised: the object it returns was laid out as a
frame-column struct, and then left the function. Its caller could never reconcile the
two, so both functions were refused and everything fell back to the interpreter.

## The six layers

1. **`return` refuses a struct.** The object-return arm is
   `WHALE_IS_ARR(t) || WHALE_IS_OBJ(t) || WHALE_IS_VAL(t)`, which does not include
   `WHALE_T_STRUCT` - deliberately, since a struct has no JSValue in a slot. But a
   struct that is *returned* does have one. Adding `WHALE_IS_STRUCT(t)` made
   `setField` compile for the first time.
2. **`_ws.wa[]` is never declared.** The `whale_ret_obj_g` arm of `OP_object` emits
   `_ws.wa[k]` and hands it to `JS_WhaleStructNew`. That arm had never been reached
   by any case in this suite, so TinyCC reported `field not found: wa` the moment it
   was. One declaration in the `_ws` struct fixes it. Latent bug, unrelated to the
   reducer.
3. **The "shallow test" misses `return {literal}`.** Fixed by accepting
   `prev_op == OP_define_field || prev_op == OP_object` as well. Now `setField`
   builds a real object, which is right.
4. **`OP_object`'s frame-column arm has no call gate.** `put_loc`'s fill requires
   `whale_call_n == 0 || pos > whale_call_pos[last]` - "nothing after this point can
   call" - but the frame-column arm only required `mono && whale_str_n == 0`.
   `step`'s `var st = {a:0,...}` sits at pos 0 with `setField` called at pos 44, so
   the literal was laid out as columns and then reassigned from a call. Adding the
   same gate stops that.
5. **Object slots were not unified between `put_loc` and `set_loc`.** `set_loc`
   already had a `loc_slot[]` mechanism (one slot per local, with
   `JS_WhaleObjSlotCopy` when they differ), but its `WHALE_IS_STRUCT` arm skipped it
   and `put_loc`'s struct arm never registered the slot. So the entry edge put `st`
   in slot 0 and the back edge allocated slot 3 - the two met at the loop exit as
   `STRUCT(0)` against `VAL(3)`, which is what `[merge]` reported.

   This is the one the user's instinct was right about: "the same variable cannot
   have two types, so the graph is wrong". It was not a merge-policy problem.

   With `loc_slot` registered in both arms the slots agree (`[merge] prev=26
   now=24`, both slot 0) and only the *kind* differs.
6. **A struct and a plain object in the same slot.** Same object, stale columns -
   the object was replaced by the assignment, so the columns must be re-read. A
   re-fill at the merge point using `JS_WhaleStructFieldNum` fixed the conflict and
   got the function compiling end to end. **But the answer was `check=0`.**

## Why it is reverted

After layer 6 both functions compile and run (`flag=0` on all 20 timed calls), and
the result is **wrong**: `check=0` against node's 249985. The generated `step` shows
the literal `{a:0,...}` emitted **inside the loop body** (`JS_WhaleObjNew` +
`JS_WhaleObjDefineNum` with the column values `L2..L6`, right after `L34`), so every
iteration rebuilds `st` and overwrites what `setField` returned. Its bytecode is at
pos 0-31, before the loop head at 34, so the emission order is wrong too - that is a
seventh layer, in the loop buffer or the block split, not in the type lattice.

The baseline is *correct and slow* (interpreter, 549ms). Six layers of change turned
that into *fast and wrong*. Correctness does not trade against speed, so it is
reverted: `0 mismatches` over all 25 cases, `whale api test: ok`.

## What to do next

- Start **from layer 4 + 5 together** (the call gate on the frame-column arm, and
  object-slot registration in all struct arms). Those two are independent of the
  scan-pass change and of the merge re-fill; layer 4 alone stops the literal being
  laid out at all, which may make layers 5-6 unnecessary.
- Then find why the literal lands inside the loop body. Check `loop_back[]` and the
  `loop_begin`/`loop_end` range against `native-control-flow-plan.md`.
- Verify with `entries` (committed this session): `immutable_update` should go from
  no entry at all to 21, and `check` must stay 249985 at every step.
- Diagnostics that earned their keep, both now committed: `WHALE_MERGE_DBG` (prints
  the conflicting types at a merge) and `[ops]` printing opcode names. Re-add
  `WHALE_FOLD_DBG` and the `[fill-slot]`/`[setloc-slot]`/`[putloc-struct]` prints if
  this is picked up again - they are what localised layers 5 and 6.
# <61>: 补丁清理、两个关键 bug、struct 统一字段协议（框架 case 的编译通路）
SOLVED + PLAN

## 先提交的（前一轮遗留）
- `5a3add3` goal.md：用户删去 AOT 与 Full/Lite，项目收窄为纯 JIT。
- `036a6f8` 未提交 WIP（deopt re-arm + pin 门槛 + 热路径 getenv 打印）原样入库——它构建绿、25 case 全对；其中 pin 门槛被下一提交证明引入了 vnode_build 的编译失败，getenv 打印随后删除。留档是为了每一步可回退。

## bug#1：vnode_build `'L3' undeclared`（commit 3964cf7 修复）
`[tokerr] failed at token 1665: ... return 0; }  L3 = JS_WhaleObjFieldNum(...)`——
不是标签丢了，是**用了没声明的变量**：prologue 的 struct 列声明（`L[var_count..+16)`）被 WIP
新加的 `whale_shape_stable` 门住不发了，而 `put_loc` 的填充路径（strn/call位置/mono 一组条件）
照样发 `L3 = JS_WhaleObjFieldNum(...)`。**同一件事的两半必须同源**：现在列变量无条件声明，
谁填充都有的用；16 个 double 是"永不发散"的全部代价。vnode_build 恢复编译（3 fn，entries=1）。

## bug#2：immutable_update 的 flag=1 deopt 退役循环（commit cf13703 修复）
症状：3 个 fn 编译成功，`[deopt] call=1 flag=0` 之后 call=2 起全 flag=1，8 次后退役，515ms ≈ 解释器。
机制：fn_3（setField，参数已按 (对象,数值) 处理）第一次调用收到**普通对象**（step 里字面量建的 st）
一切正常；从某次起收到的是 `JS_WhaleStructNew` 造的 **WHALE_STRUCT**，而
`JS_WhaleObjFieldPtr` 第 8446 行 `class_id != JS_CLASS_OBJECT → NULL` → fp resolve 失败 → deopt。
**修法 = struct 走同一个 `JSValue*` 字段协议**：
- `JS_WhaleObjShape` 对 struct 返回 WhaleStruct 指针本身（列一次分配永不移动；两个 struct
  绝不能共享键——struct 对象没有普通属性，p->shape 会给所有 struct 同一个空 shape）；
- `JS_WhaleObjFieldPtr/ObjFieldNum` 对 struct 查列返回 `&ws->data[col]`；
- `JS_SetPropertyInternal2` 加写 hook：列写进列，列外名字进 dynamic map——否则中途 deopt 后
  解释器 `st.a=...` 会 find_own_property 落空、长出普通属性，而读永远先答列 → 静默丢写。
配套（各自由 31 秒首测量身定制）：
- **offset map 从 JS 对象改平行 atom 数组**：原来每列每次调用一次 `JS_GetProperty`，
  Redux 循环每迭代 ~15 次，是最大单项；
- **dynamic map 惰性创建**（三个 hook 都已把 undefined 当"尚无 dynamic"）；
- **`js_whale_struct_gc_mark` 注册**：列里的 JSValue 与 dynamic 都由 malloc 的 struct 持有，
  不在 JSObject prop 数组里，mark_children 看不见——**vnode 的 `props` 字段此前会被 GC 回收**；
- **运行时 getenv 全部缓存**（`whale_str_stat_on`/`whale_step_dbg_on`/DeoptNote 的 dbg）：
  `whale_struct_col` 每查一次列调一次 getenv、每迭代九次，31 秒里最大头。
结果：immutable_update entries=1000920/1000920 全进编译代码，468-538ms ≈ 解释器速度——
每迭代一次真实跨函数调用（CallValue 建帧+wrapper）+ struct 三次分配（ws/atoms/data）。
要快起来靠内联（A4）或参数特化（D3）让 setField 不再经过引擎建帧，以及把 StructNew 的
三次 malloc 合成一两次——都已列入路线。

## 补丁问题（commit 3964cf7 + 8031b05）
- 生成器不再发任何 `register`（pin1/pin2 选择、whale_str_need 的 16 个 register 列、
  induction[] 扫描、whale_nloops 计数全部删除——它们唯一的读者就是 pin 门）。
- TCC 侧 pin 簇八处全部恢复上游：whale_pin_alloc/地图/PinEnter/PinLeave、save_reg 的
  spill-on-call、get_reg/get_reg_ex 的 pin 测试、gv 读捷径、gen_op 操作数归一化、vstore 写捷径、
  TOK_REGISTER 忽略如初、`Sym.pin_reg` 从 tcc.h 删除。
- 热路径 getenv 调试打印删除（JS_WhaleArgGetObj / whale_jit_unbox_num——每次 JIT 调用入口执行）。
**删除的意外收益**：closure 57→10ms（3.3×node）、props_diff 125→22ms（5.5×node）——
16 个 `register` 声明消失后 TCC 的寄存器分配恢复正常。notes 里"pin 收益 5-12%"的测量
是在 pin 只覆盖无循环小函数时取得的；对真正带对象工作的函数它是纯负担。

## 测量注记（又见热身陷阱）
同一二进制直接连跑 props_diff = 65-124ms 且漂移；runner（先 probe 预热 + repeat 取最小）
稳定 21-22ms。**A/B 差异里顺序效应可以伪装成因果**——先跑的组吃冷启动。以 runner 为准。

## gcc 后端注记
- 必须给全路径：`WHALE_JIT_CC=gcc`（裸名）时 `whale_cc_dir` 得到空串，命令拼成
  `"\gendef.exe"` → `gendef produced no .def` → **静默退回解释器**（bitops 545ms vs TCC 150ms）。
  全路径下数值循环全面反超（nested 87→18、locals 102→24、minmax 110→35、sieve 165→70 ≈ C -O1）。
- **objfields+gcc = 17ms，反超 node 19ms**。`c_native/objfields.c` 103ms 的"天花板"被
  `fmod(i,7)` 的完整 double 语义压低——gcc 不能把 libm 调用整数化，而 JIT 的守卫 idiv
  （两操作数为小整数时走 idiv，否则回 fmod，<57> 实测守卫免费）在 JS 语义内合法更快。
  "比 C 基线还快"不神秘，是**基线被 double 语义束缚**；真正该修的是把 c_native 基线里的
  fmod 换成 C 里的 `(long)i % 7` 才是同口径天花板。
- props_diff 在 gcc 后端 614ms（TCC 22ms）：大函数体在 gcc 默认 flags 下疑似无 -O，
  一切过内存。后端轮再处理（WHALE_JIT_CFLAGS 实验 + 默认值检查）。

## 下一步（具体方案，按杠杆排序）
1. **call_method（string/arr_methods 的总闸）**：bail=5174。receiver 在 `sp[-2-argc]`、
   method 在 `sp[-1-argc]`，都 park 进 obj slot，method 用 `WHALE_VAL_T`，
   复用 `JS_WhaleCallValue(func_slot=method, argc, arg_base=receiver,...)`——
   call2 fallback 的参数 parking 已在，缺的只是把 receiver 算作第 0 个参数。
2. **add_loc 的 opaque 通路（stringcat）**：bail=4278。`s += "x"` 两侧都是 VAL slot 时
   降为 `JS_WhaleBinopVal(add, slot(s), slot(v), out)` 再写回 local——写回要走 loc_slot
   统一（<60> 第五层同款坑：入口边和回边的槽号必须一致），不是 10 行的事，但方案闭合。
3. **数组参数 D3-lite（array/list_reconcile/nested2d/objshape 的共同缺口）**：
   wrapper 的参数守卫加"packed array"读法（`JS_WhaleArrPtr` 已在），配 `get_length`
   helper（当前没有）、`get_array_el/put_array_el` 的对象值版（objshape 的 escaped()
   在 put_array_el 带对象值处 bail=4023）、嵌套 ARR 元素（`g[r]` 读出的是数组）。
4. **字符串装箱**（用户方向）：字符串运算全部退化到引擎 C 函数（js_add_slow 一类
   已经存在，`JS_WhaleBinopVal` 已接）——瓶颈不在算，在**跨 helper 的装箱纪律**：
   值只停 obj slot、进出口一次装箱，避免"helper 间来回 tag 判断"。做 1/2 时顺手定规约。
5. **StructNew 分配合并**：ws+atoms+data 三次 malloc → 一次（尾随分配），每 struct
   少两次堆往返；Redux 形状每次迭代都建 struct，这是它的剩余大头之一。
6. gcc 后端默认 flags 检查（props_diff 614ms 疑案）与裸名陷阱报错显式化。

## 状态
`8031b05` 绿：`whale api test: ok`，25/25 case 与 node 一致（runner 口径 0 mismatch，
含 vnode_build/immutable_update 恢复编译）。temp/ 下的 gen_*.c dump 不入 git。

# <62>: env 重构、25/25 全编译、BinopVal 栈约定、fresh struct 直写
SOLVED + PLAN

## env 重构（commit 164cfcf）
- `JS_WhaleSetJitCc(driver, flags)`（whalejs.h）替代 WHALE_JIT_CC/CFLAGS/KEEP。全局存储在
  whale_jit.c（后端和 TCC state 本就进程级）。**必须完整路径**：裸 `gcc` 让 whale_cc_dir 得空串，
  拼出 `\gendef.exe` 静默失败退回解释器。
- 75 个诊断读取点经 `whale_dbg_env()` 宏；release 构建编译为常量 0。`--whale_debug=y` 进
  WHALE_DEBUG。字符串型（STEP_OFF/EXPR_MAX/OBJ_TEST/JIT_OBJ/JIT_SRC/ISA）各自 #ifdef。
- WHALE_ENTER_COUNT 不条件化：它是 perftest 输出协议的一部分（entries 列），不是打印。

## 两个静默错误答案 bug（commit bb3d440）
- **JS_WhaleBinopVal 的操作数布局**：`js_add_slow`/`js_binary_arith_slow` 等的契约是
  解释器操作数栈——读 `sp[-2]`、`sp[-1]`，结果写 `sp[-2]`。传本地 `JSValue v[2]` 意味着它们
  读写**数组之前的 C 栈残留**：算了一堆垃圾、结果丢在数组外、v[0] 原样回槽——**每个 opaque
  binop 都是静默无操作**。stringcat 拼 200 次长度仍 0 就是它。修法：操作数放 v[2]/v[3]，传 v+4。
- **cur_sp = NULL 实验残留**：入口处把 cur_sp 设 NULL（注释自称 TEMPORARY EXPERIMENT，
  <55> 排除的是崩溃症状，代码没恢复）→ GC 对 JIT 帧什么都不标 → 长循环里分配触发的 GC
  回收槽内对象。恢复 `obj_slots + WHALE_OBJ_SLOTS`（<46> 的语义）。
- 定位手法值得记：单次调用永远解释器（编译在热阈触发、当次退回解释器）——**测编译版必须
  两次调用**；逐步打印链条（pushSlot → run → get_length → outNum → binop rc/len）每一环
  都"正确"、答案仍错时，正确性洞在**函数间约定**（栈布局/GC 窗口），不在任何单环。

## 四条新通路（commit 5a891fb）
- `call_method/tail_call_method`：`JS_WhaleCallMethodValue(ctx, func_slot, this_slot, argc,
  arg_base, ret_slot)`——receiver park 后作 this，其余同 CallValue。emit 时注意每臂闭自己的
  括号（SlotCopy 臂漏一个 `)` = `')' expected`）。
- `add_loc` opaque：BinopVal 写回 local 自己的槽，槽身份不变、合并不受影响。
- `put_array_el` 对象值：`JS_WhaleArrSetVal(ctx, slot, idx, val_slot)`，镜像 ArrSetNum。
- 数组参数：scan 前瞻 `whale_feeds_array_use`（.length 相邻；get_array_el 前 1 推；
  put_array_el 前 2 推）→ wrapper 用 `JS_WhaleArrBind`（查 class）→ `get_arg` 类型
  WHALE_T_ARR → 现有 packed 路径接管。

## fresh struct 直写 + makeProps 的 mono 陷阱（commit dd3f863）
- `JS_WhaleStructDataPtr` 一次取列数组；fresh 窗口（StructNew 后、遇 call/跳转目标前）内
  define_field/put_field 数值写直接 `_sdp[col].tag=8; _sdp[col].u.float64=v;`。
- **mono 陷阱**：ret_obj StructNew arm 原要求 `mono`（profiler 单 shape 判定），但工厂函数
  没有访问 site → sites_n=0 → mono 恒假 → 落入 ObjNew 兜底 → 后续 StructSetNum 拒普通对象
  → **每次调用必 deopt、8 次退役、全程解释器**。布局来源改为字面量自身的 define_field atoms
  （scan 已收集 `whale_def_atoms`），兜底 arm 配 `JS_WhaleObjDefineNum` + plain 位图。
- 教训：同一 helper 配不同对象种类 = deopt on rails；新 arm 落地时先问"每个前置条件的
  反例是什么形状"。

## 性能与下一步
25/25 编译、0 mismatch。props_diff 17ms（entries 39808）、vnode_build 10ms、
immutable_update 491ms（1,000,920/1,000,920 进编译代码）。vnode/diff 剩余成本 = **每调用
包装**（CheckStack + ArgGetObj×2 + `_ws` 2.7KB memset + DeoptNote + memo 查找）≈ 执行体
20 倍：突破口是内联（A4，已列第一梯队）与 `_ws` 按需定尺寸（scan 已知 field sites/
obj slots 实数）。objfields+gcc 17ms 已反超 node；c_native/objfields.c 的 fmod 基线口径
问题记 <61>。gcc 后端走 JS_WhaleSetJitCc API（完整路径）；stdio 写 .o/.a + TCC 解析加载的
后端形态待后端轮（先验证 MinGW COFF 对象与 TCC PE 装载器的兼容性）。

## 状态
`dd3f863` 绿：`whale api test: ok`、25/25 case 0 mismatch、git 干净。

# <63>: objshape 分相、逃逸对象真 struct、单块分配、immutable 的剩余成本结构
SOLVED + 测量 + PLAN

## objshape 分相（1.2M 迭代/相，temp/obj_phase.js）
twoShapes 47 / growsShape 16 / writeOrder 20 / **escaped 430**（node：15/11/5/19）。
growsShape 1.5x、writeOrder 4x 都已可接受；**escaped 一相占全案 80%**。

## 逃逸修复（commit 46f84c1）
`OP_put_array_el` → `whale_ret_obj_g = 1`：数组活得比迭代长，帧列布局每次存入都要
物化成真对象（ObjNew + 每列 ObjDefineNum，~350ns/迭代）。现在字面量直接 StructNew，
fresh 窗口内写直走 `_sdp[col]`，**读也直走**（get_field fresh 分支：tag 8/0 内联，
否则 `JS_WhaleObjValueNum` deopt），数组存的是引用。对象身份真实、别名安全。

## 单块分配
`JS_WhaleStructNew`：header + atoms + data 一次 js_mallocz，finalizer 一次 free。
immutable_update 每迭代一次 StructNew → **491 → 392ms**（200 万次 malloc 消失）。

## 诚实数据
escaped 自身 413-680ms（改前 430）：**没变快**。墙 = `JS_NewObjectClass`（JSObject+
shape 构建、`js_trigger_gc` 检查）+ 每分配的 GC 扫描 + 每迭代一次 finalizer。node 18ms
靠逃逸分析——`t` 没有真正分配。要在"对象必须真实存在"（out 数组持有）的前提下逼近，
需要分配器级工程：轻量类对象路径（跳过 shape/proprop）或真新生代 bump 分配。

## immutable_update 392ms 的成本结构（1M 次迭代）
每迭代：1 次 `JS_WhaleCallValue` → `JS_Call`（**全帧装配 ~250ns**，占一半以上）；
`VarRefGetObj` 每迭代重 park setField（指针整个调用期不变，应入口 park 一次）；
`fver++` 每代 3 个 fp site 各重解析（JS_WhaleObjShape + FieldPtr = 6 helper）；
setField 内部：StructNew（单块后已便宜）+ fresh 直写 + ret_obj 装箱。
**下一步（按值排序）**：
1. **compiled-to-compiled fast call**：JS_WhaleCallValue 里查 callee 已编译（bytecode
   function + whale_jit_entry）→ 绕过 JS_Call 的解释器帧装配，手工建最小帧
   （current_stack_frame/arg_buf 拷贝/var_refs/prev_frame 四件事）+ 调 entry + 拆帧。
   预期 -150~200ms。这是"switch 直跳 JIT"大方向的第一小步（notes <61> 混合编译评估）。
2. var_ref 入口 park 一次（~20ms）。
3. fver 代内 shape 共享（~60ms）：fver++ 后首个访问解析并把 shape 存进代槽，同代
   其它 site 直接比代槽——把 6 helper 降到 4。
4. 分配器：JS_NewObjectClass 的 struct 专用轻量路径。
## 状态
`46f84c1` 绿：25/25 编译、0 mismatch。objshape 481ms（entries 48 = 四相全编译）、
immutable_update 392ms。

# <64>: compiled-to-compiled fast call、closure 值 prologue park 一次
SOLVED

## fast call（commit ae43f3e）
`whale_fast_call(ctx, func, argc, args, ret)`：callee 已编译时（bytecode function +
whale_jit_entry），绕过 JS_Call 的解释器路径——类分发、whale_on_call（已编译后只喂计数）、
参数拷贝（**caller 的 obj slots 本就是 dup 过、GC 可见的 parked 值，直接作为 callee 的
arg_buf 借用，零拷贝**）。保留的只有不能跳的：帧本体（生成代码的每个 helper 都读它：
obj slots、栈检查、异常展开）。deopt 语义完全保持："整帧从顶部重跑" = 丢弃快帧 +
全新 JS_Call（re-arm 计数同样维护）；deopt==3 的换读法在快帧内原位重入（快帧本就是
正确读法的帧）。约束：argc == arg_count 且 var_ref_count == 0，否则落 JS_Call。
JS_WhaleCallValue 先试 fast-call，失败无副作用地落 JS_Call。

**测量：immutable_update 392 → 342ms**。比预期的 -150 小：事后归因，JS_Call 对已编译
callee 的开销里帧装配占大头且无法省，真正省掉的是分发 + hook + 参数拷贝 ≈ 50ns/调用。
**教训：估算"调用开销"前先分清"帧装配（不可省）"与"路径杂费（可省）"。**

## closure 值 prologue park（同 commit）
closure reading 的调用目标槽（setField）原来**每循环迭代**重 park 一次
（JS_WhaleVarRefGetObj + dup）。捕获值在整个调用期不可变——entry 自检依赖同一事实——
现改为 prologue 一次 park。为此 scan 增设 `var_ref_val[]`（closure reading 的值读标记）：
原 `var_ref_used[]` 的语义收窄为 entry self-check（仅 self reading）。
**注意**：单遍 emit 无法预知，park 语句必须发在 prologue 结构段（scan 已知 var_ref_val），
emit 处只推 VAL(槽)。

## 测量口径备忘（第三次）
直跑（cold）与 runner（warm+取最小）差 5-6×：closure 直跑 52ms / runner 11ms。
**裁定一律用 runner**；直跑只用于看诊断输出。

## 状态
`ae43f3e` 绿：25/25 编译、0 mismatch。immutable_update 342ms、objshape 462、
props_diff 15、vnode_build 10。

# <65>: var_ref 推测内联——机制建成、实测变慢、单点关闭
SOLVED（机制）/ UNRESOLVED（为什么慢）

## 机制（commit 41a5ebc，全部在树内）
- 引擎：`JS_WhaleVarRefFuncInfo(ctx, idx, &fn, &info)` 编译期解析（current_stack_frame
  正是**正在编译**的帧，其 var_refs 活着——这是"编译期拿运行时值"的合法通道）；
  `JS_WhaleVrExpectAdd(fn)` 把期望函数注册进静态表返回索引（token 发射器没有 64 位
  字面量，指针以索引 travels）；`JS_WhaleVarRefIsFn(ctx, var_refs, idx, expect_slot)`
  是运行期守卫。
- 类型 `WHALE_T_VRFN=11`，字节编 **var_ref 索引**（不是槽号）。
- `whale_inline_body` 参数类型继承调用点栈槽（原硬编码 WHALE_T_NUM——setField(o,v)
  第一个 get_field 就死）；内联体的 OP_object 走 StructNew+DataPtr+fresh 直写
  （列 = callee 自己的 define_field 序，调用点预扫）；whale_str_atoms/n/fresh 提为
  文件级 static（与 whale_objs/whale_fields 同模式），gen emit 入口重置。

## 三个踩过的坑（修完机制才通）
1. **cur[idx] 一格两用**：var_ref idx 与 local idx 是不同空间，get_var_ref closure
   分支写 `cur[idx]=VRFN` 污染了 local（step 的 `i` 变成函数类型 → 参数被拒）。
2. **gen 没有 var_refs**：编译期解析走 current_stack_frame 的 cur_func。
3. **前置声明顺序**：whale_str_col 定义在 inline_body 后，inline_body 现在要调它。

## 实测：变慢，已单点关闭（`if (0 && JS_WhaleVarRefFuncInfo...)`）
immutable_update 342 → 549ms 且 entries 归零；objfields 55 → 554ms entries 归零。
关闭解析后全恢复：immutable **327ms**、25/25、0 mismatch。**为什么慢未诊断**——
下一步第一件事：dump 两个读法的内联流（self reading 的 get_var_ref 分支同样受
scan 改动影响；vr-inline 的 truncate/re-type 与 general 分支的 fresh 物化段交互是
首要嫌疑）。

## 好消息
- fast-call + park（<64>）在无干扰下稳态兑现：immutable 342 → 327ms。
- escaped 的 StructNew-per-iteration 实测 612 vs 物化 430 —— 逃逸标记回退，
  原因注释在 scan（墙是 JS_NewObjectClass，不是列布局）。
- whale_str_* static 化 + inline_body 的 StructNew/fresh 直写保留（内联转正时即用）。

## 状态
`41a5ebc` 绿：25/25 编译、0 mismatch、immutable 327ms。

# <66 续>: closure 槽 = 存储单元（pval），一次取、直读写
SOLVED

`JS_WhaleVarRefPval(ctx, var_refs, idx)` 返回闭包槽的存储 cell（`JSValue *`），prologue
按 `var_ref_rw[]`（scan 新增：值读或写）每槽取一次；get_var_ref 变成 `_p->tag` 检查的
直接读，put_var_ref 变成 tag 检查（证明旧值是 immediate，无需 JS_FreeValue）后的直接写。
`n = n + 1` 在编译的闭包循环里 = load + add + store，零 helper、零装箱——
**"普通函数 + 闭包上下文指针"的形态，正是用户问的**；比"出入口装箱"更进一步：循环路径
完全无箱。

坑：`wt_id("_p"); wt_int(idx);` 发出**两个** token（`_p` `0`），TCC 读作 `_p` 未声明——
组合名必须走 `wt_slot("_p", idx)`（与 L0/_p0 同机制）；另有三处 `return 0 }` 漏分号
（同一批编辑引入）。

closure 10-11 → **9ms**（node 3ms；剩余 = 每迭代 2 次 tag 分支 + 访存，TCC 无寄存器
提升）。**进一步**：闭包计数器的循环内寄存器化（回边写回）需要 LICM 级证明，暂缓。

# <66 续>: TCC/ELS 调研 + vr-inline 的正确性修复清单（机制禁用中）
TIPS + UNRESOLVED

## vr-inline 三个正确性修复（已做但因性能回退随分支丢弃，重新启用时需重做）
1. `whale_inline_body` 的 get_arg 参数类型必须**继承调用点栈槽**（`t = cur[var_count +
   argmap[a]]`），原硬编码 WHALE_T_NUM 让 setField(o, v) 的第一个 get_field 就拒。
2. get_var_ref closure 分支**不得写 cur[idx]**：var_ref idx 与 local idx 是不同空间，
   写 cur[idx]=VRFN 污染了 local（step 的 `i` 被类型化成函数 → 参数被拒 → 549ms 全解释）。
3. 编译期解析走 `JS_WhaleVarRefFuncInfo(ctx, idx, ...)`（current_stack_frame 是正在
   编译的帧，其 var_refs 活着）。
即使三修复齐全，实测仍 549ms + entries 归零（fn_2 编译失败）——第二个读法的 get_field
拒绝链（ta=1）未根因。**下轮第一步**：修完三坑后 dump 第二读法的内联 token 流，逐 token
对照 —— refuses 链在 get_field ta 继承（ caller 栈槽类型在解析时刻的值需要打印确认）。

## TCC/ELS 调研（用户问：延时生成 + ELS 强制扭转 spill 是否可行）
### 现状（源码量化）
tccgen.c 6986 行，**单遍解析即发码**：`g()` 被调 **1487** 次、`o()` **3270** 次
（共 ~4800 个直发字节调用点），寄存器管理仅 `get_reg` 8 处 / `save_reg` 4 处。
**没有 IR、没有活跃分析、没有寄存器压力模型**——局部变量永远走内存（VT_LVAL 每次
load/store），寄存器只活在一个表达式内部。这是 TCC 的设计选择（编译速度优先），
不是 bug。
### ELS 需要什么
1. **延时生成（两遍化）**：全部 4800 个发码点改为"记录操作"—— 即一个线性 IR
   （load_local/store_local/const/binop/call/branch/label，词表 ~40 个 op）。
2. **活跃分析**：对 IR 线性扫（反向）得每个 local 的活区间。
3. **ELS 分配**：区间按起点排序，扫过活跃列表分配 xmm/r 续借寄存器，用尽时
   spill 区间终点最远者。对热循环（2-6 个活跃值、14 个 xmm）绰绰有余。
4. **重放发码**：spill 决策已在 IR 上，重放是直译。
### 难度
- **路线 A（改 tccgen 本体）**：`gen_modrm` 把"选寄存器/寻址模式"与"发字节"熔合在
  x86_64-gen.c（2238 行）内部；ELS 要在发码**前**知道寄存器归属 → 必须解耦寻址
  选择 → 动到 x86_64 全部指令的寻址决策。估计 2-4 周 + 全 C 前端回归面
  （初始化器/位域/VLA/long long/浮点转换……）。**不建议**。
- **路线 B（生成器侧 ELS，推荐）**：我们的"输入"不是任意 C——是 whale_jit.c 生成的
  **规则子集**（直通数值代码 + 分支 + helper 调用），而且**全函数体已在缓冲里**
  （loop_begin/loop_end 的 token 缓冲、<58> 的循环表）。在生成器层直接产出
  带 ELS 寄存器分配的线性 IR、绕过 tccgen 直发 x86（或以"预分配寄存器"的形式
  馈 TCC），工作量 ≈ 一个小型专用后端（估 800-1500 行），避开 tccgen 纠缠。
  **这正是 fast-call/内联一路的自然延伸：由我们控制的编译管线。**
### 编译时长代价
- TCC 单函数编译 < 1ms（热阈触发的编译尖峰从未被观测为可测停顿）。
- ELS 增量：op 缓冲 + 线性活跃扫 + 区间排序，对 50-200 op 的函数体是**微秒级**
  ——编译时长代价可忽略。
- 对照：gcc 后端单函数几十-几百 ms（进程 spawn + 编译 + 链接），质量最高但
  浏览器冷启动不可接受。**ELS-in-generator 的意义 = gcc 质量的分配 + TCC 量级的
  延迟**，正好是当前缺的中间档。
### 收益上限（参照 gcc 后端实测）
bitops 150→70、nested 87→18、locals 102→24、minmax 110→35、sieve 165→70 ——
数值循环 1.5-4x。ELS 只覆盖分配维度（指令选择/调度仍差），诚实预估**热循环
1.3-2x**。而 fast-call/内联（immutable 327ms 一路）对框架形状的收益更大，
**优先级低于内联，高于 TCC 本体改造**。
### 建议排序
1. gcc 后端做**热路径专用**（冷启动 TCC + 热点换 gcc 的两级 JIT——WHALE_JIT_CC
   已在，加"再编译"机制即可，先测延迟分布）。
2. 若延迟不可接受 → 生成器侧 ELS（路线 B）。
3. tccgen 本体改造（路线 A）永不。
## 状态
`effa8f0` 基线绿：25/25 编译、0 mismatch、immutable 367ms、props_diff 16ms。
本轮探索性改动（vr-inline 修复 + _ws 选择性清零）已从工作区移除，详细修复清单
与本节结论保留在此，供下轮继续。

# <69>: vr-inline 机制修通、测量决定休眠 + 附带的 getenv 每调用探测 bug
SOLVED（机制）/ 休眠（默认关）+ SOLVED（getenv）

## 五个真 bug（上轮"三个修复也没转正"的答案）
dump 第二读法 fn_2 的诊断，一层剥一层，共五个，每修一个露下一个：
1. **内联体 `JS_WhaleStructNew` 少发 `ctx`**：主循环发 4 参、内联体发 3 参
   → `too few arguments to function`，fn_2 在 TinyCC 层编译失败，整函数退回
   CallValue 路径。这就是上轮 549ms + entries 归零的直接原因。
2. **token 提交交错**：内联体各 op 各自 `fold_commit` 已直写输出，调用点最后又
   `fold_commit(fm)` 把 [fm, now)（守卫 + 体 token + 悬空的 get_arg 表达式）
   重复追加 → 守卫错位到体后、StructNew 块重复、`get_arg` 残片 `(L0)` 泄漏成残句
   → `';' expected (got "if")`。修法：体改写独立缓冲
   （`whale_inline_tok/num/len`，仿 loop_begin/loop_end 的输出切换），成功才由
   调用点先提交守卫、再 `whale_inline_out_append()` 追加体；失败整段丢弃。
   同一改动覆盖 cpool 内联路径（它原来也是 `fold_commit(fm)` 重拷整段）。
3. **`whale_vr_expect_slot` 从未写入**（恒 -1）→ `JS_WhaleVarRefIsFn` 恒返回 0 →
   编好也每次入口 deopt、8 次退役。补落库。症状与上轮"守卫永远失败"同款，
   但位置不同（上轮是读错数组，这次是没存期望值）。
4. **内联体复读调用点参数槽**：`{a:o.a,...}` 第一个 get_field 把装对象的栈槽
   cur 重写成 NUM，后续参数读到 `ta=1`（NUM）→ 直接拒绝。修法：入口快照参数的
   类型 + 表达式 + 物化状态（`whale_inline_argt/argexpr/argmat/argname`），
   get_arg 只读快照。
5. **失败体污染调用点**：`OP_object` 消耗 obj 槽、`whale_str_fresh` 等残留，
   fallback 的 general call 误读。快照恢复（cur/vexpr/vmat/vname/objs/fresh，
   另加此前漏的 `whale_vserial`）。

## 测量结论：机制对，但转正反而慢 → 休眠
- 内联全通：fn_2 `[vr-inline] rtype=40`、零 deopt、check=249985 正确。
- 但 immutable 慢 40~80ms/1M 迭代（327→~400），entries 920（编过但 deopt？不，
  是 policy：见下）。守卫成本已用开关隔离证明为零。
- helper 聚合计数（临时插桩，已移除）：基线每迭代 fieldptr×7 + slotcopy×2 +
  structnew/dataptr 各 1 + callvalue×1；vr 版 fieldptr×4 + shape×3 + 守卫 + 同样
  的物化/拷贝。**总数相等（8/迭代）**——差额不在 helper 次数。
- 生成码逐行对过：逻辑无冗余，6 次读里 4 次内联 + 2 次保留检查都是必要的
  （st.a 跨迭代重读必须重解析）。
- 所以差额 = **TCC 代码质量**：体内 ~40 条语句全走内存（VT_LVAL 每次
  load/store，无寄存器分配），相对快帧 ~50ns 的开销是净负。objfields
  （轻体内联，只差 2ms）佐证：体越重越亏。
- 处置：`WHALE_VR_INLINE` 开关门控（whale_dbg_env，debug 构建可用，
  release 硬关）。开关开 25/25 全对——正确性已验证，转正只等生成器侧 ELS
  给体内语句寄存器（<66 续> 路线 B 的自然前置条件）。

## 附带：getenv 每调用探测 bug（预存在，顺手修）
- `JS_WhaleDeoptNote` 在 `WHALE_ENTER_COUNT` 未设置时，**每次编译函数返回都
  走一次 getenv**（100 万次 × ~2.7µs ≈ +2.7s）。runner 恒设该变量所以历史测量
  全"干净"；直接跑二进制（用户跑自己的 fib.js、裸跑 case）全在赔这个钱。
- 现象曾伪装成"我的改动让默认路径退化"（同一二进制同一开关，裸跑 3000ms、
  `WHALE_ENTER_COUNT=1` 跑 320ms)。定位手法：entries 计数 + 两变量对照，
  排除 stderr 去向/僵尸进程/构建配置后只剩环境变量本身。
- 修法：单次探测（`whale_entry_probed`），与 <61> 的"热路径 getenv 全缓存"
  同款。裸跑 immutable 2950→~310ms。
- 教训：**runner 口径 ≠ 用户口径**。任何只在 runner 下测的数字，都要补一次
  裸跑对照；反之亦然（直跑受控制台/后台负载影响，裁决仍以 runner 为准）。

## 验证（release 默认态）
`whale api test: ok`；25/25 全对，0 mismatch；immutable 316ms/entries=1000920
= 基线；debug 构建开关开 25/25 全对（immutable ~415ms/entries=920，预期内）。
## 状态
绿，待提交。下一步按 <68> 排序：分配器（struct 轻量路径/新生代）→ 两级 JIT
→ 生成器侧 ELS（vr-inline 转正的前置）。

# <70>: 两级 JIT 的 gcc 一半：按函数指定的 gcc -c 后端，TCC 进程内链接
SOLVED

## 用户要求（原话）
两级 JIT 只需要允许 gcc，并且可以使用函数指定即可（默认不需要打开）；
stdio 输出 .o 内容，用 TCC 本身的能力加载。

## 落地形态（四件）
1. **`pe_load_coff`（tccpe.c，新，~200 行）**：把 `gcc -c` 产的 PE-COFF .o
   翻译进 TCC 内部表示——ALLOC 段（code/data/rodata，含 `/N` 长段名解析）
   并入同名 TCC section；defined 符号 `set_elf_sym` 登记新地址；undefined
   符号留 SHN_UNDEF（`pe_build_imports` 按现有链绑定 `tcc_add_symbol` 地址，
   无需手动处理）；重定位只收 REL32→`R_X86_64_PC32`。`.pdata/.xdata`
   unwind 与 debug 段跳过（JIT 调用不需要）。
2. **PC 语义差 4 字节（真 bug，独立探针 + 生成码对照确认）**：COFF REL32 的
   P 是字段末尾（r_offset+4），TCC 的 `relocate()` 用 `addr = sh_addr +
   r_offset`（其自家 codegen 靠预置 -4 补偿）。gcc 留字段为 0，所以 -4 走
   RELA addend（`put_elf_reloca(..., -4)`）——`relocate_reset()` 在二次链接时
   把字段清零，补在数据里的 -4 会被洗掉，走 addend 则安然无恙。
3. **`tcc_add_file` 不经过 `JS_WhaleCompileTokens`**：其尾声的
   `runtime_main = NULL` 清理（<35>）走不到 → 下次 `pe_output_file` 误判为
   "run a program"，`pe_add_runtime` 注入 `__runmain` + CRT 库 → 一堆
   `undefined symbol 'main'/'IAT.*'`。修法：iface 新导出
   `JS_WhaleTccClearRuntimeMain`，`whale_gcc_compile` 在 load 后调用。
4. **按函数指定**：`JSFunctionBytecode.whale_jit_gcc`（默认 0）+
   `JSWhaleFuncInfo.prefer_gcc`（三处填充点透传：whale_on_call/Cpool/VarRef）
   + 公开 `JS_WhaleSetJitCcForFunction(ctx, func)`。选择条件是
   `prefer_gcc && driver 已设`——默认全走 TCC，无行为变化。
   旧 `.dll` 路径整套退役：gendef/dlltool/`whale_gcc_prepare`/`LoadLibrary`
   （~100 行删除）。源码仍走 stdin pipe 不落盘；`.o` 落 TEMP 临时文件
   （as 拒绝 `-o -`，实测确认），load 后删除（`WHALE_JIT_KEEP` 可保留做
   `objdump -d`）。

## 证伪记录（省得后人重走）
- `pe_load_file` 只认 `.def`/`.res`/DLL；`tcc_load_coff` 是 C67 DSP 专用
  （只注册符号不搬运 section），不可复用。
- `gcc -c -o -` 不可用：as 报 `can't open a bfd on stdout`。

## 测量（whale_test 第 9/10 段，同形状 fibi/fibg，400×fib(30)）
- TCC：4ms（含编译）；gcc：252ms（含一次编译）/ **纯执行 1ms**。
- 即 gcc -O2 执行体约 3-4 倍于 TCC（与 <61>/<67> 的 1.5-4x 一致），
  但单函数编译成本 ~250ms（spawn + -O2 + as）。结论：小函数不划算，
  大函数/长热点才值——"热点再编译"（ distant future，按 <68> 排序在分配器
  之后）按需启用时，用执行时长/编译成本比做门限，别按调用次数。

## 验证
`whale api test: ok`（第 10 段 gcc-o 端到端：标记→编译→400 次调用全对→
compiled 增长；无 driver 时跳过）；25/25 全对，0 mismatch；默认态 immutable
314ms = 基线。
## 状态
绿，待提交。两级 JIT 剩下一半（热点再编译机制 + 门限策略）按 <68> 排在分配器之后。

# <71>: 编译阈值分级 + clang 说明 + 技术债清单
SOLVED（阈值）+ TIPS（清单）

## 阈值分级（用户要求：嵌套大循环早编译，纯数学运算晚编译）
- 旧：一刀切 `WHALE_JIT_HOT_THRESHOLD=100`（调用+回边统一计数）。
- 新：`whale_hot_threshold(b)`——`loop_count>=2`→10；单循环且字节码>64→20；
  单循环小体→50；无循环→100（保持）。依据在编译前已知（<58> 循环表 +
  byte_code_len），阈值跟随预期收益而非统一。
- 三处调用点全换（`whale_on_call`/`whale_note_backedge`/`whale_prof_maybe_arm`）；
  profiling 窗口（8 次）是绝对值，阈值 10 时在 count=2 打开，样本仍够 mono 判定。
- `[hot]` 诊断（`WHALE_JIT_DUMP` 下打印 count/threshold/loops/bytelen）。
- 验证：`nested`（loops=2）在 count=11 编译，`plain`（loops=1, len=22）在
  count=51 编译，语义全对；25/25 全绿，稳态数字不变（perftest 都是长跑，
  阈值只影响短跑/冷启动行为——这正是它的价值所在）。

## clang（用户纠正：gcc/clang 都行，只需允许）
- driver 本来就是路径参数，传 clang 路径即 clang 后端；所传 flags
  （`-x c - -c -w -fno-strict-aliasing -Wno-error=incompatible-pointer-types
  -O2 -o`）全是 clang 兼容的，无需代码改动。
- 本机无 clang，实测待有环境时补。`JS_WhaleSetJitCc` 的注释已写"gcc/clang"。

## 三级编译（用户：不一定，看 ELS 落地后的差距）
- 暂不做。当前两级（TCC 默认 + gcc 按函数启用）已覆盖"允许"的全部语义；
  ELS 落地后若 TCC 与 gcc 的差距从 3-4x 拉大到 ELS 也追不上的程度，再评估
  中间级（gcc-O1 快速升级 / ELS 常驻 + gcc 热点换装）是否有必要。
- "热点再编译"（ distant future）需按**执行时长/编译成本比**设门限（<70> 已记），
  不能按调用次数——阈值分级解决的是"何时首次编译"，再编译解决的是"何时升级"，
  两件事别混。

## 技术债清单（截至本轮，仍欠的）
1. **单例 TCCState 代码累积无上限**：每次编译追加 section，`whale_jit_seq`
   递增，长期运行只增不减（<35> 设计如此，当时只解决正确性）。需按代回收
   或上限驱逐。
2. **ELS 未落地**（<66 续> 路线 B，估 800-1500 行）：数值循环 1.3-2x 的来源，
   也是 vr-inline 转正的前置。用户已确认方向，排在分配器之后。
3. **vr-inline 休眠**（<69>）：机制全通，`WHALE_VR_INLINE` 开可用，转正等 ELS。
4. **热点再编译机制**：门限策略已明确（执行/编译比），机制未建。
5. **分配器**（最大单项，<68> 排序第一）：struct 轻量路径或新生代 bump，
   escaped 相（objshape 80% 时间）的墙是 `JS_NewObjectClass` + GC 扫描。
6. **memo 只按 self 读法建**（<48>）：closure 读法无 memo，对应形状无缓存。
7. **裸 driver 名诊断弱**：`JS_WhaleSetJitCc("gcc",...)` 靠 PATH 解析，失败静默
   回退解释器（<61> 同款陷阱的 API 版）——应显式报错或在 stats 里记 failed。
8. **longjmp 安全**：`tcc_error` 是 longjmp，`pe_load_coff` 内 `tcc_malloc` 的
   失败路径用返回值而非异常（已如此），但 `set_elf_sym` 的"defined twice"
   是 `tcc_error_noabort`（只记数不跳）——多次加载同一符号名时的行为待实测
   确认（目前每次函数名带 seq 计数器，天然避开）。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch；immutable 339ms = 基线。
## 状态
绿，待提交。

# <72>: 多 Runtime 安全确认 + struct free list（分配器第一步）
SOLVED（多 Runtime）+ SOLVED（free list）

## 多 Runtime（用户要求确认）
- 审计结论：`whale_jit_state_ensure()` 有 `if (whale_jit_state) return 1`
  守卫，**不会多次 `tcc_new()`**；`whale_vr_expect` 表单调追加（指针全局
  唯一，满 64 返回 -1 已有检查）；host 符号地址进程通用；`.o` 临时文件名
  seq 全局唯一；`whale_jit_backend` 按函数切换（同步编译，用完即覆盖）。
- 实测：`temp/multi_rt.c`（不入库）双 Runtime 交替编译+调用，
  `rt1=rt2=832040 OK`。
- 约束（记入技术债）：**多线程并行编译不安全**——生成器暂存全是进程全局。
  QuickJS 本身要求多线程用独立 Runtime，而我们的 JIT 让独立 Runtime 的
  并行编译也会互踩。同线程多 Runtime 交替则安全（编译同步不重入）。

## struct free list（按 Runtime 隔离，满足"别跨 Runtime 共享"）
- 位置：`JSRuntime.whale_struct_free[16]` + 计数，`JS_NewRuntime` 零初始化
  自带，`JS_FreeRuntime` 在 `JS_RunGC` 后手动排空（`in_free` 守卫让排空走
  正常释放；直接调 `free_object` 会重回链 + 对已摘链节点 `list_del`）。
- 回链（`free_object` 入口）：struct + 无 dynamic + 桶深<64。跳过 shape/prop/
  finalizer 全套释放；prop 块先释（空 shape 的无内容），字段复作链指针。
- 取用（`StructNew`）：出链 + `refcount 0->1` + 重挂 GC 链表 + 释旧列值 +
  全列重写（与新路径同一 `fill:`）。
- 两个真 bug（都是"复用对象与新对象状态不对等"）：
  1. **复用对象不在 GC 链表**（`remove_gc_object` 摘了，新路径的
     `add_gc_object` 没走）→ 列新值被 GC 回收 → 启动期即崩。补挂链。
  2. **旧列值泄漏**（回链跳过 finalizer 的列释放，fill 直接覆盖）→ 补释放。
- 测量：immutable_update 339→245ms（-28%）；objshape 580→528ms（-9%，
  escaped 仍是 GC 扫描主导，符合 <63> 结论——free list 省分配，不省扫描）。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch；multi_rt OK。
## 状态
绿，待提交。分配器下一步：GC 扫描侧（escaped 的剩余 90%）需分代或标记优化，
属大工程，按 <68> 排在 ELS 之前评估。

# <73>: ELS 上限实测 + c1 死变量 + pe_isafunc 真 bug
SOLVED（测量+两个 bug）/ LICM 未动手

## ELS 收益上限（同函数、唯一变量是后端，gcc-o vs TCC）
| case | TCC | gcc-O2 | 倍数 | 形状 |
|---|---|---|---|---|
| nested | 108 | 18 | 6.0x | 长表达式链 |
| locals | 118 | 26 | 4.5x | 局部轮转 |
| minmax | 170 | 36 | 4.7x | 嵌套归约 |
| arith | 123 | 69 | 1.8x | 单链 |
| bitops | 181 | 108 | 1.7x | 位运算 |

nested/locals 的 4-6x 主要来自 gcc 全局优化（LICM/强度削弱/调度），不是纯
分配——ELS（纯分配）能稳拿的是 arith/bitops 档的 1.5-2x。pin 只值 5-12%
（历史数据）的原因同：语句级 load/store 已是最简，变量级 pin 省无可省。

## bug#1：`c1/c2` 死变量（<13> 的常量传播从没工作过）
fast path 的 `assign_cnt[d2]==1 && c1` 里 `c1` 从声明就没被赋值过（搜全文件
无一处写入）→ 条件恒假 → `lo_c` 永不置位 → 字面量初始化永不传播。改为
`(k1==WLD_INT || k1==WLD_REAL)`（analyze 的 kind 本来就是常量判据）。
修后 nested 体内出现 `(5.5+1.5)`、`(2.5*3.5)` 等字面量；nested 90→82ms。
配套：`lo_c/lo_cv` 提文件 static（`whale_lo_c/lo_cv`），`local_inline` 对
deferred 常量发字面量而非变量。

## bug#2：`pe_isafunc` 在 x86_64 下恒假（上轮的 bits 崩溃根因）
`ELF32_R_INFO(sym, R_386_PC32)` 的 32 位打包永不等于 64 位 `r_info` →
函数全判成数据 → 不建 thunk jmp → call 跳进 IAT 数据 → 野解码。
fibg 靠地址值解码运气没崩，bits 的 SIGFPE 才暴露。改为
`ELFW(R_INFO)(sym, R_X86_64_PC32)`。修后 bits gcc-o 108ms vs TCC 181ms。
教训：TCC 上游的 PE 代码有 32/64 位混用残留，动它的符号逻辑要逐行验位宽。

## 删掉的死代码
`whale_const` 全套（栈槽字面量标记）：`push_const; put_loc` 全被 peephole 2
截获，通用路径的消费永不命中。与 fast path 重叠，留着是负担。

## LICM 未动手（下轮）
常量传播修完 nested 只 -9%：TCC 自己会折叠字面量表达式，真瓶颈是变量访存
+ 长链求值。真正的 LICM（`loop_end` 处 token 重排，不变式移缓冲外）插入点
已确认（`loop_begin/loop_end` + `WhaleLoop` 缓冲），但需要解析 token 流识别
不变式，本轮没动手。顺序：LICM → pin/ELS 分配（转正 vr-inline 的前置不变）。

## bug#3：`set_loc` 漏统计 → 常量传播静默错答案（本轮最危险）
- 症状：`mixed`（`x=1.0` 初始化 + 循环内 `x=x+y*2`）JIT 给 `60000005`，
  node/qjs 给 NaN（指数发散后 `Inf-Inf`）。`x` 被当成单赋值常量 `1` 重写循环体。
- 根因：`assign_cnt` 只统计 `put_loc/inc_dec`，不统计 `set_loc`（存值又留栈）。
  `x` 的循环体内写是 `set_loc0` → 只计初始化 1 次 → 误判单赋值 → 错误传播。
  与 <13> 的 `inc_loc` 漏统计**同类**（"传播类优化漏算一个写操作"）。
- 修法：预扫描加 `OP_set_loc0-3/8/set_loc` 全形态计数。
- 教训：任何"写局部"的 opcode（put/set/inc/dec）都必须进 `assign_cnt`；
  加新写 opcode 时先查这张表。`mixed` 必须进回归（它是唯一的 set_loc 覆盖）。

# <74>: peephole 常量折叠 + live 拒绝的常量例外
SOLVED（折叠）/ 部分生效

## 常量折叠（peephole 1 的双字面量算术）
- 两操作数全是 `WLD_INT/REAL` 字面量 → 编译期 double 求值发字面量。
  IEEE 语义与运行时一致（`1/0=Inf`、`0/0=NaN`），无 int 除零陷阱（全程 double）。
  只做 `+-*/`，不碰比较（bool 的 `?:` 形态收益小）。
- 结果若是小整数发 `wt_int` + 记 `lo_c`（折叠结果也是常量，可继续传播）。

## `live` 拒绝的常量例外
- `whale_load_analyze` 对 deferred 局部一律拒绝（防 stale 读），导致 peephole 1
  对含常量的表达式永不匹配——折叠代码 unreachable。
- 修法：`live` 但 `lo_c` 置位 → 不拒绝（常量读出字面量，无 stale 可言）。
- 配套：`local_inline` 对 `!live` 且 `lo_c` 的局部发字面量（deferred 常量在
  flush 后也要读出字面量，而非变量）。

## 效果与边界
- nested 体内出现 `(5.5+1.5)`、`(2.5*3.5)` 等字面量（之前是变量引用）；
  nested 90→73ms。其余数值 case 持平——TCC 自己会折叠字面量表达式，
  真瓶颈仍是变量访存 + 长链求值（LICM 的事，本轮没动手）。
- `(L0+L1)` 未折叠：`a/b` 的 pending 是变量（`local_set` 记录时栈顶是变量
  引用，非字面量）→ `local_inline` 发变量。这是 deferred 变量转发的形状，
  不是常量问题；要动它得让 `local_set` 透传 `lo_c`（下轮）。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch。
## 状态
绿，待提交。

# <77>: LICM 地基（inv 追踪 + 外提框架，未生效待调试）
PARTIAL

## 做的事
- `WhaleVExpr.inv`：字面量推送/`get_arg` 置 1，binop 取操作数 AND（通用路径 +
  peephole 1），其余默认 0。`fold_set` 清零（调用点置位）。
- `fold_materialize`：循环内 + `inv` → 语句发外层输出（循环头标签前），后读
  内联变量如常。只做栈槽物化（`_ws.v<n>`），不做 deferred 局部。
- 语义安全：`inv=0` 默认保守（不外提）；`inv=1` 要求操作数全字面量/参数/常量。

## 未生效
nested 的 `_ws.v[0]/v[1]` 没外提（体与之前相同）。`inv` 链在某处断了：
`(L0+L1)` 的 peephole 1 没匹配（`ok1/ok2` 因 `live` 拒绝，尽管 `lo_c` 已置位——
`live` 拒绝的修复只放行了 analyze，但 peephole 的 `ok1` 还要 `type_is_num` 等，
或者 `p2/p3` 错位）→ 通用路径（`local_inline` 字面量，不记 `inv`）→ 链断。
下轮加 `inv` 诊断（每步打印槽号 + inv），看断链点。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch（不外提是保守正确，零风险）。
## 状态
绿，待提交。

# <80>: inv 继承灾难（11 case 静默错答案）与删除
SOLVED（回退）/ 教训

## 症状
11/25 case 静默错答案：`array`/`immutable`/`object`/`objfields` 直接 `check=0`，
`bitops`/`branch`/`loop_sum`/`sieve` 等数值全错。共同形状是"循环只跑首轮值"。

## 根因
`fold_set` 的 `inv` 继承（`whale_fold_src`）：纯转发继承源槽 `inv`。但 `call`/
helper 结果的 `fold_set` 后无覆盖 → 继承最后内联的参数源 → 参数常量则 `inv=1`
→ `fold_materialize` 把**调用搬出循环** → 每轮读首轮值。`get_field` 等内存读
同理（结果 `inv` 继承源，不变性无从谈起）。

## 修法
删继承（`fold_set` 默认 0，`whale_fold_src` 删除）。`inv` 只由能证明的点置位：
字面量推送、`get_arg`、binop AND（含 `fold_mod`/位运算）、`get_loc` 的 `lo_c`。
单目运算（`neg`/`lnot`）不需处理（`fold_set` 后无覆盖，但源是自身旧值，透传正确；
`OP_plus` 恒等不调 `fold_set`，保持）。
全量 0 mismatch 恢复。

## 教训
`inv` 这类"可外提"标记只能由**构造点**置位（字面量、参数、AND），绝不能由
**转发**继承——转发不知道目标语义（调用/内存读的结果不是纯函数）。与 `lo_c`
（只记字面量推送，不记表达式结果）同例。`fold_inline` 后 `fold_set` 的点凡
无显式 `inv` 覆盖的，一律按可变处理。

## 附带
- runner 的 entries 列部分空白（`immutable_update` 等）：check 全对，无 MISMATCH，
  功能正常；疑 `WHALE_ENTRIES` 行解析问题，待查，不阻塞。
- immutable 245ms（基线），fib 21ms（噪声），mixed 204ms（基线）。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch。
## 状态
绿，待提交。

# <78>: CStruct 频率化 + object free list + 线性表结论
SOLVED + SOLVED + TIPS

## shape 频率化（用户设计）
- 旧：`mono` 要求全 site 单形状（100% 一致），布局取并集。`objshape` 的
  growsShape 多态污染全函数 → escaped 等单形状对象也被拒之门外。
- 新：`whale_sites_freq` 接受 ≤4 形状的 site（明确放弃的仍拒绝）；
  `whale_layout_from_sites` 按**去重形状覆盖率**过滤（≥40% 进布局，按覆盖率
  降序，截断 16），gcc 二编（`prefer_gcc`）门限降到 0（全量已知属性）。
- 中间踩坑：第一版按 site 数算覆盖率，objfields 188ms（3 倍退化）——tag/key/depth
  各只占 2/10 site 被滤掉。改形状级（同形状多 site 只计一次）后恢复 63ms。
  教训："40% 引用"指形状覆盖，不是 site 数；site 数会被访问点数量稀释。

## object free list（物化路径的复用）
- escaped 走 `ObjNew`（plain 对象，`ret_obj` 标记被 <63> 故意关闭），不在 <72>
  的 struct free list 覆盖内。每迭代一次 `JSObject` + shape 生长 + 旧对象全释。
- `JSRuntime.whale_obj_free` 单链（ObjNew 全是空对象，无需分桶）：
  回链条件（`JS_CLASS_OBJECT` + 空 shape + 无 weak）+ `ObjNew` 取用（重挂 GC +
  `DefineNum` 照常生长）+ FreeRuntime 排空。按 Runtime 隔离，同 struct。
- objshape 577→530ms（-8%），immutable 285→236ms（-17%）。

## 线性表结论（用户问：16 分界，统一入口两套内存？）
- 实测：`col_lookup` 在热路径**零调用**（`_fs/_fp` 缓存全命中，`WHALE_STR_STAT`
  无一行输出）——线性扫只在冷路径（shape 变化/first touch）跑。
- 结论：**保持线性，不引入哈希**。冷路径 16 次比较可忽略，热路径已缓存；
  两套内存 + 统一入口的复杂度不值。除非将来 prof 显示扫表进 top（届时直接看
  `col_lookup` 计数器）。
- 附带：`col_lookup` 计数器此前因 shell 环境变量传参问题误报"零调用"，用
  ps1 脚本确认后仍是零——计数器本身正常，是传参姿势错（`set` 在 bash 仿真下
  无效，要用 ps1 的 `$env:`）。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch；nested 72ms（保持）。
## 状态
绿，待提交。

# <79>: 布局去上限 + object free list 全复用
SOLVED + SOLVED

## 布局去上限（用户：16 不是截断线，静态越多越好）
- `WHALE_STRUCT_FIELDS`(16) 只保留作 free list 分桶数；布局、栈数组、prologue
  声明、`StructNew`/`Native` 检查全改 `WHALE_ATOM_MAX`(32)。
- prologue 按固定 32 声明（不能按 `fn` 动态：布局在 emit 中期才定，prologue 在
  开头，读不到——固定全宽是 <61> "声明与使用同源" 的延续，32 doubles 可接受）。
- free list 分桶保持 16（超限不复用，直接释放；宽对象少，复用收益小）。
- perftest 列数全 <16，无性能变化（预期内）；价值在未来宽对象。

## object free list 全复用（hit=100%）
- 第一版只收空对象 → escaped（死时已有 v/u）永不回链 → `hit=0`。
- 改收非空普通对象：归还时释 prop 内容值 + 置 UNDEFINED（accessor 另走），
  链指针存 `u.opaque`（普通对象未用；`p->prop` 存的是 prop 块，不能丢），
  shape/prop 块保留。复用时 `DefineNum` 覆盖（shape 已有属性则无生长）。
- `hit=8 miss=0`（2 的幂次采样）。objshape 577→529ms，immutable 285→236ms。
- immutable 236-285ms 徘徊是 GC 时机噪声（repeat=5 回 249ms），非回归。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch。
## 状态
绿，待提交。

# <76>: fib 退化排查 + memo 关断回退 + A 收尾评估
TIPS + 未解

## fib 8→16ms（用户指出）
- `by_flag[8]` 死计数器删除（只递增从不读取）：省 1-2ms，保留。`whale_entry_total`
  保留（perftest entries 列依赖）。
- memo 关断实验（`pure1 && !selfref`）：fib 16→349ms（掉回解释器）+ entries 12→31
  （deopt 暴涨），**已回退**。原因未明：memo 只跳过执行不改变语义，关断不应引发
  deopt；怀疑 `selfref` 为真时的 entry 选择与 memo 有隐性交互（entry2 编译 + memo
  缺失的组合），待查。记入技术债，不阻塞。
- 回退后 19ms（噪声）。8ms 时代是 memo 命中 + 更少的 wrapper 开销；当前 16-19ms
  是稳态。B（ELS/LICM）落地后重测。

## A 收尾评估：做不了（需 ident 反查）
- `(L0+L1)` 未折叠是 deferred 变量转发：`local_set` 记录时栈顶是 `L0` 变量引用，
  `local_inline` 原样发出。要透传 `lo_c`，需从 token 反查变量号。
- `wt_slot` 发的是单 ident token（`tok_alloc("L0")` 的 id），生成器侧无 ident 表，
  无法从 `gw_tok[off]` 反查 n。`fold_inline`/`local_set` 都不知道栈顶变量号。
- 结论：A 收尾需要 ident 反查基础设施，排到 ELS（路线 B 建线性 IR 时自然有符号表）
  之后。本轮不做，诚实记录。

## 验证
`whale api test: ok`；fib 19ms（稳态）。
## 状态
绿，待提交。

# <81>: LICM 转正（inv 链修通 + 外提生效）
SOLVED

## 三处断链（全用 trace 实证定位，非猜测）
1. **变量读 inv 恒 0**：`fold_set` 默认 0 后，任何含变量叶子的表达式 inv=0——
   nested 的 `a..e` 全是循环内无写的变量，整链是 0。修法：`whale_loop_written`
  （pass-1 back-edge 表建成后，对每循环头扫 `[head,back]` 收集写 local 位集，
   `(len+1)*var_count` 字节，`out:` 处释放）+ `whale_local_invariant(idx)`
   （只查最内层循环：外提目标是直接外层输出，每外层迭代重算，外层写也正确）。
   用点三处：get_loc 变量读分支、get_loc0_loc1 双分支、peephole-1 的 SLOT
   操作数（经新 `whale_load_inv` helper，INT/REAL/ARG 照旧 1）。
   写点判定 `whale_writes_local`：put/set/add/inc/dec 全形态 + check 系 +
   set_loc_uninitialized；其余（close/make/using_dispose/get）不是写——即使漏判，
   JIT 在那些 opcode 上本就会 bail，written-set 用不上，安全。
2. **mat-before-inv 时序**：`fold_set` 内超长即 materialize，外提读 inv，
   但调用点的 AND 回写在 `fold_set` 返回后——nested 的 div（len=33>24）
   实测 `[mat] inv=0` 而随后 `[gb] inv=1`。修法：`fold_set(k,mark,inv)` 改传参，
   25 调用点全改（字面量传 1，算术传 AND，内存读/call 传 0，local 读先算 `li`
   再传）。`whale_load_inv` 同时给 peephole-1 去重。
3. **单目/vr-inline 的滞后回写并入参数**：neg/lnot/not/inc/dec 的 `inv` 局部量、
   vr-inline get_arg 的快照 inv、binop 的双操作数 AND（旧 forwarding 只取右
   操作数，左循环变量+右常量会误判不变——史上最险的一处），全部改在
   `fold_set` 调用时传入。
- 配套：`local_set(d,mark,inv)` 加参（put/set_loc 传栈顶 inv，add/inc_loc 传 0，
  后者表达式含 `L<idx>` 变量叶）；vmat=1 的 10 处（call 结果/field/array/
  length/var_ref/global/SELF/OBJ）显式 `inv=0`（stale 继承的 <80> 同类）；
  `OP_swap` 的结构体整体交换 inv 跟走（正确，不动）；`OP_dup` 同理。
- 时序铁证：trace 显示 `[gb] sp=1 a=1 b=1 inv=1` 后 `[mat] inv=0` 再 `[gb] inv=1`，
  改传参后 `[mat] inv=1` + `[hoist]` 两次，check 不变。

## 效果
- nested 88-94→70-74ms（repeat=3 稳态 70ms，外提 2 次：33-token 与 28-token
  子表达式各一次）；其余 case 基线噪声内（immutable 290、objshape 629 均为
  已知波动位）；25/25 全对，0 mismatch；`whale api test: ok`。
- 所有诊断 trace（`[mat]/[hoist]/[pe1]/[gb]/[loc]`）已删，无残留。

## 教训
- "回写"模式与"内部触发"不兼容：凡在 `fold_set` 内部读 `inv` 做决定的
  （materialize 外提），`inv` 必须是参数，不能是事后回写。
- 按 body 范围（而非当前位置之后）判写是正确的：回边使"写在读之后"仍影响
  下次迭代读值，body 范围判正好等价于"下次迭代是否相同"。
- trace 证据链：`[loc]` 确认 lo_c 置位正常 → `[pe1]` 确认 peephole inv=1 →
  `[gb]` 全 1 但 `[mat]` 为 0 → 锁定 fold_set 内外时序。三段式隔离缺一不可。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch；nested 70ms（repeat=3）。
## 状态
绿，待提交。下一步：ELS 路线 B（线性 IR + 分配，<67> 已调研）。

# <82>: ELS 机制恢复 + 范围收缩（local pin hang 待查）
PARTIAL（机制就绪，决策收缩，零收益零回归）

## 探针结论（动手前先证伪）
- `WHALE_EXPR_MAX` 24→200：nested/minmax/locals 反而慢 14-29%，只有 bitops
  快 16%。瓶颈不是"语句太多"，而是 TCC 在表达式内部也 spill——单纯合并语句
  无用（与 <74> 的 24 最优结论一致）。
- TCC 语句边界 spill 是架构性的：`save_regs` 6 调用点全是语义必需（调用保存、
  控制流合并），`vstore` 赋值必写内存，删任何一个都是错代码。TCC 侧无小改。
- 用户纠正：gcc 不能假设存在，默认路径永远是 TCC，ELS 收益人人可拿；
  且对象操作多的函数也有大量数值运算，不能因有对象操作就放弃 ELS。

## 恢复的机制（TCC 侧 8 处 + 生成器侧决策）
- TCC 侧（照抄删除 diff 的最终正确形态，避开 5 轮试错的坑）：
  `whale_pin_alloc`（float 池 ≤4 个）+ 偏移反查表（不用 sym，声明初始化与读
  的符号不一致坑）+ PinEnter/Leave + save_reg（spill 照做）+ get_reg×2 跳过
  + gv（类检查 + move 回退 + 调用后重载）+ gen_op 归一化 + vstore shortcut
  + TOK_REGISTER + decl 分配 + gen_function 重置。
- 生成器侧 ELS 决策：`whale_loop_heat`（循环体内读+1/写+2）选 top-4，排除
  对象 local（`whale_loc_obj`）与单赋值（`assign_cnt==1`，lo_c 字面量无需 pin）；
  `_wv` pin（fold_materialize 在循环体内用 pin 位，不用 `_ws.v` 数组）。
- gcc 侧：renderer  strips `register`（<61> 教训，props_diff 125→830ms 不重演）。

## 修好的三个真 bug
1. **Invalidate 放错 gfunc_call 版本**：加在 ELF 版（1347 行）而非 PE 版（921 行），
   Windows 根本不编译它 → 调用后 pin 值失效且 slot 未更新 → bitops 错（287）、
   stringcat 错。移到 PE 版后两者全对。
2. **save_reg_upstack 无条件清 validity bit**：`?:`/`&&` 的非调用 save 也清，
   pin 全失效（nested 反汇编全走内存）。改只在 gfunc_call 清（真破坏），
   gv 读时重载（slot 权威）→ nested 70→31ms。
3. **跨函数 pin 表误匹配**：core/wrapper 共享表但帧布局不同（已改每函数重置，
   后证伪非主因，保留重置）。

## 范围收缩（诚实记录未解的）
- **local pin hang**（未解，但证据链已闭合）：
  - 四个归一化假设全部被实验证伪：手工改 vtop[].r（删）、gen_op 入口 gv 全路径
    归一化（试）、vstore 后保持 lvalue 形态（试）、跨函数表隔离（试）——全不解决。
  - **gv trace 决定性证据**（WHALE_PIN_DBG）：alloc 成功（loc=-104/-112 →
    xmm5/xmm4）、Invalidate 写回清位正常、**gv 重载路径完全正常**（mask 从 0 →
    0x100000 → 0x300000 逐位恢复）、wrapper 编译时表重置（n=0）也正常。
    **pin 机制本身在 core 内无一处错误。**
  - 新假设（下轮第一嫌疑）：**pin 占用寄存器池后，TCC 表达式求值的浮点临时不够，
    `get_reg(RC_FLOAT)` 返回 -1，某个调用方把 -1 当寄存器号用** → 生成坏代码
    （死循环/hang.asm 的 i++ 消失 + 比较错乱与此吻合：坏代码形态）。验证方法：
    在 `get_reg` 返回 -1 处加打印/断言，pin 开跑 nested 即抓现行；若坐实，修法是
    get_reg 失败时 spill 一个 pin（pin 位当"可回收"用，正如 cb87bf1 当年对
    exhaustion 的处理），或者池再留一个余量。
  - 另：31ms 的旧收益数据出自 debug 构建，release 下从未复现；local pin 的
    hang 在纯 release 下确认（minmax 与 nested 都是）。
- **_wv pin + 调用 = NaN**：_wv 的 slot 从不写入（materialize 只写寄存器），
  调用后重载读旧值（immutable_update NaN）。决策：_wv pin 只在无调用函数开
  （`whale_loop_has_call`，纯数值白名单 `whale_is_pure_num_op` 判定）。
- 当前效果：25/25 全对 0 mismatch；nested 76、bitops 154、array 53、minmax 108
  全是基线噪声内（零收益零回归）。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch（repeat=1 全量 + repeat=3 关键 case）。
## 状态
绿，已提交。下一步：fib per-call 开销、vr-inline 转正、vnode/immutable 追 v8。

# <83>: fib 开销修复 + vr-inline 体门限转正 + 范围收紧
SOLVED（开销+门限）/ PARTIAL（追 v8 未竟）

## fib 21→19ms：_wv 无条件声明的 per-call 税
- `_wv0..7` 无条件声明 → 无循环函数（递归 fib）每调用付 8 个零初始化 store。
  修法：只在 `loop_count>0 && !has_call` 时声明（条件与 materialize 分配点一致）。
- 另确认：heat 统计/pin 表全是编译期成本，per-call 只剩 `_ws` 零初始化与
  prologue locals（一直有）。fib 稳在 19-20ms。

## vr-inline 默认开 + 体门限（转正）
- setField(52B) 内联亏 60%（386 vs 254ms），createElement(20B) 内联也亏
  （27 vs 11ms）——当前 TCC 代码质量下任何实测体都亏。门限 12B（等于关闭但
  保留路径：体质改善后调参即开，无需重构）。
- 为 vnode 补内联体能力（对象值 `ObjDefineValue` 分支 + `OP_null` case），
  但门限已挡住 createElement（实测亏则挡，对行为无影响，能力保留）。
- clen 诊断（`[vr-inline]/[inline]` 打印体长）保留，调门限时用。
- 25/25 全对，immutable 240（-4%）、vnode 11（基线）、全量零退化。

## minmax +20% 回退与修复
- `_wv` pin 在嵌套循环有害（两层争 4 深池，spill 反增）：minmax 109→129ms。
- 修法：_wv 声明与分配统一收紧到 `loop_count == 1 && !has_call`
  （之前声明是 `>0`、分配是 `depth>0`，条件不一致）。minmax 回 110ms。

## 追 v8 现状（诚实）
| case | v8 | whalejs | 倍数 | 路径 |
|---|---|---|---|---|
| vnode_build | 2-3 | 11 | ~4x | createElement 内联亏，需体质改善（ELS local pin）或压 fast-call |
| immutable_update | 14-29 | 240 | ~10x | setField(52) 内联亏；field shape 特化未做 |
| nested | 15-27 | 70 | ~3x | 等 local pin hang 修好（下轮专项，证据链完整） |
- vnode/immutable 的缺口不在调用（fast-call 已最优），在**体质**（对象创建 +
  字段访问的语句质量）——正是 ELS local pin 要解决的。顺序正确：先修 hang，
  再谈追赶。

## 预存问题（非本轮引入）
- 退出崩溃（immutable/vnode，check 对）：e778eab 干净树亦崩，runner 口径
  （只解析 WHALE_BENCH 行）无影响，用户直跑可见。待查（疑 FreeRuntime 期）。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch；fib 19-20、nested 70、minmax 110、
vnode 11、immutable 240（repeat=3 关键 case + repeat=1 全量）。
## 状态
绿，待提交。下一步：local pin hang 专项（get_reg -1 回收）。

# <84>: fib per-call 开销 + local pin 重开验证
SOLVED（开销）/ CONFIRMED-OFF（pin）

## fib 21→14ms：三处 per-call 税
1. **`_ws.sv[128]` 死存储删除**：frame-column 布局后列住在 `L[]` 变量，sv 数组
   无一处读写（只剩声明）→ 删声明，_ws 缩 1KB。fib 20→16ms（best-of-5）。
   `sp[128]` 保留（presence 还在用）。
2. `_wv0..7` 条件声明（<83> 已做）：无循环函数不再付 8 个零初始化。
3. struct 列 32 个零初始化门控（<83> 已做）：fib 无对象，跳过。
- 三管齐下 fib 稳在 14-16ms（repeat=1 全量 14ms，best-of-5 16ms）。7ms 时代
  的剩余差距在 `_ws` 整体（v/wa/fs/fp/fv/ap/an/ai/op/on/oi，fib 大部分不用但
  `{0}` 整块初始化）——拆 struct 是大改，下轮评估按需清零 vs 拆分。
- 用户原话验证：2ms 波动是误差，7ms 是地板；中间加入的内容（sv 死存储、
  无条件 _wv/struct 列声明）确实是 per-call 税，已清。

## local pin 重开验证（get_reg 回收已就位）
- 重开 local pin → **连 whale_test 的 fibi 都 hang**（卡在 `[tcc]` 之后）。
  get_reg -1 回收假设被证伪：hang 与池耗尽无关（whale_test 的小函数也 hang）。
- 决策：local pin 保持关闭。hang 的完整证据链见 <82>；新补充：单 pin 也 hang、
  重开即 hang 测试套。gen_op 入口的 gv 全路径归一化保留在树上（无害：无 pin
  时 pin_find 全返回 -1，零行为变化），gen_sse_modrm 的 pin 识别保留（同理）。
- 下轮专项方向修正：不再是"池耗尽"，而是 pin 变量进入表达式求值后的 vstack
  状态错乱（hang.asm：i++ 消失 + 比较错乱）。从 `i++` 的 `L6 = L6 + 1` 在
  pin 下的 vstore→gen_op→vstore 序列单步跟（token 级，不是猜）。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch；fib 14、nested 75、minmax 118、
vnode 11、immutable 247（repeat=1 全量）。
## 状态
绿，待提交。下一步：local pin hang 专项（表达式求值 vstack 跟踪）。

# <85>: local pin 根因链闭合（hang→错答案）+ pool 预算定案
PARTIAL（根因锁定，修复留待下轮）/ fib 15ms 稳住

## 最小复现与证据链（WHALE_PIN_DBG trace + 反汇编对齐）
- 最小复现：fibi(30)×100（`temp/fibi_min.js`）。编译完成后 hang；单次调用
  不触发（阈值 50）。`temp/fibi_hang.asm` 为 hang 时的完整反汇编。
- pin 分配 trace：4 个 register 声明（4 L + 8 _wv 中前 4 个成功）→ **a/b/i/t
  全 pin（xmm5/4/3/2），_wv 全部失败**。之前以为 _wv 先声明抢 pool——顺序
  实际是 L 在前。
- **pool 耗尽坐实**：a+b、i<n、重载 n 全要 xmm，4 pin 占满 → `get_reg`
  返回 -1。加回收后不 hang 了，但**变成错答案**（fibi 返回 1 而非 832040）：
  回收要求"整个 vstack 无引用"太保守，表达式中间态引用 pr 时回收失败。
- 2-pin 预算（pool 上限 2，`whale_pin_alloc` 改）：不 hang，仍错答案。
  vstore shortcut 把新值留在 pin 寄存器不写内存，而 gv 读在 vstack 重排
  （`?:` 的 save_regs、归一化的 gv）之后重载 stale slot → `a=b; b=t` 丢更新。
  **读写不对称是机制级缺陷**：vstore 假设"值在 pr，gv 下次走 shortcut"，
  但 gv 并不总是走。
- 结论：local pin 保持关闭。机制的各部件单独验证全对（alloc/重载/
  Invalidate/回收），组合语义有一个未解的洞。gen_op 归一化、modrm pin
  识别、回收逻辑都保留在树上（无 pin 时零行为变化），下轮从
  "vstore 后 vtop 形态与 gv 读路径的交互"单步跟（token 级）。

## pool 预算定案
- `whale_pin_alloc` 上限 **2**（6 个 RC_FLOAT 中留 4 个给 TCC 表达式求值）。
- `_wv` 声明 8 个但仅前 2 个带 `register`（与 pool 一致）。
- 此预算下 `_wv` pin（fold 临时值，单写点）保持开启且全量验证正确。

## 用户提醒记录：immutable 的正确形态（前端语义，非本轮）
- `st = setField(st, v)` 与 `{...items, k: v}` 应做成**原地修改 + 触发
  proxy/setter**，而不是真建新对象——shape 没变就不该走慢路径。
- 现状 235ms 的构成：每迭代 StructNew（free list 已优化）+ 快帧调用。
  上限改造 = 引擎侧"同 shape 原地写"的语义层（record update 融合），
  与 CStruct 的列存储天然契合。记为方向，优先级在 local pin 之后。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch；fib 15、immutable 235、
vnode 11、nested 77（repeat=1 全量）。
## 状态
绿，待提交。下一步：local pin 的 vstore/gv 不对称修复；immutable 原地更新。

# <86>: local pin 定案（trace 全链）+ _wv 2-pin 验证 + 两个检查方向
SOLVED（定案）/ 方向记录

## trace 全链（WHALE_PIN_DBG：RESET/alloc/gv/Invalidate 四点）
- 顺序：RESET(core) → alloc ×2（-96/-104，reg 21/20）→ [core body 的 gv 读
  全是 -64/-72/-80/-88（L0-3），pr=-1 ✓ L 无 pin] → RESET(wrapper) → 16×
  Invalidate n=0。
- **gv 读的偏移与 alloc 的偏移完全错开**：alloc 的是 _wv0/1（L 之后声明），
  gv 读的是 L0-3。之前"alloc 成功但 find 失败"的矛盾不存在——alloc 的
  根本不是 L。上一轮"fibi 输出 1"跑的是 **L pin 开 + 2 预算**的中间态。
- 当前配置（L 无 pin + _wv 2 pin）：fibi_min 832040 ✓、whale_test 全绿、
  25/25 全对 0 mismatch、minmax 110（恢复基线）。

## 定案
- **L pin 判死刑**：4/2/1 pin、回收、归一化、lvalue 形态全部试过，要么 hang
  要么错答案。vstore 只写寄存器 + gv 在 vstack 重排后读内存的不对称是
  机制级的，在 TCC 的 vstore/gv/expr_cond 交互里修不动（每轮修一个露一个）。
  nested 的收益等换思路：生成器直接做寄存器分配（线性 IR + 直接发 token/
  指令，绕开 vstore 机制），那是 <67> 路线 B 的正题。
- **_wv pin（2 个）保留**：fold 临时值单写点、生存期不跨调用，语义安全，
  全量验证正确。这是 ELS 在 TCC 机制下能安全拿到的部分。

## 用户提的两个检查方向（记录）
1. **nested 77ms vs v8 15ms**：多重表达式，怀疑前端融合（语句合并/链式
   求值）或后端质量不到位。LICM 已做；下一步看 fold 链是否把整个表达式
   融成最少语句（`whale_expr_max=24` 的物化点是否过密）。
2. **vnode 11ms vs v8 3ms**：怀疑 CStruct 创建的固定流程开销（StructNew 的
   header+atoms+data 单块分配 + offset map + GC 挂链）。值得 profile
   `JS_WhaleStructNew` 的每次成本构成，看有没有可省的固定步骤。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch；fib 15、minmax 110、
immutable 242、vnode 11、nested 74（repeat=1 全量）。
## 状态
绿，已提交 720092b。下一步：nested 融合检查 / vnode StructNew 开销 /
生成器侧分配。

# <87>: nested 融合与 vnode 调用链 profile（零代码改动）+ 线性 IR 计划
SOLVED（profile）/ PLAN（线性 IR）

## nested：融合已完成，瓶颈是 TCC 语句级 spill（证据：nested_src.c）
- `deep()` 的循环体在生成源码里就是**两条语句**：
  `_wv0 = (LICM 外提的两段常量)；_wv1 = (..._wv0...)；L5 = _wv1；L6++`。
  33-token 与 28-token 子表达式各外提一次（<81> 的 2 hoists），`whale_expr_max=24`
  的物化点没有过密——反而正是 LICM 把 24 切出来的。
- 循环内每迭代的真实工作：2 次 `_wv` 写 + 4 次 L 读写（`L5/L6` 各一读一写）。
  对照 gcc-O2 同函数 6x（<73>），数值循环的 1.5-4x 上限里 ELS 只占分配维度。
- 结论：前端无事可做。nested 的剩余差距等**生成器侧线性 IR 分配**
  （见下），不再调 expr_max / 融合。

## vnode：createElement 11-18ms 的构成（证据：create_src.c ×3 份生成源码）
- `createElement(type,props)` 本体的编译形态已是最优：StructNew + `_sdp` 直写
  3 列（type/props 的 double 化 + key 的 ObjDefineValue），无 helper 扫描。
- 循环体（closure-reading，fn_1/fn_2）每迭代的调用链实测：
  props `{id,cls}` 走**普通对象路径**（ObjNew + DefineNum + ObjSlotAtom +
  BinopVal + DefineValue）→ createElement 经 CallValue/fast-call →
  返回 struct → `v.type+v.props.id` 经 **ObjFieldNum ×5（含保留的 L3/L5
  死读）** → `+` 经 StepRun fallback。约 15 个 helper/迭代，其中一半是
  **props 小对象自身的创建与销毁**（cls 字符串拼接 + BinopVal）。
- 关键反证：`object` case（同 loop800 万次对象读写）4ms 反超 node 25ms——
  同一套 helper 在"对象不出循环"的形状下是赢的。vnode 慢不在 StructNew
  （free-list 命中 + 单块分配已兑现），而在**每迭代 2 个对象的分配 +
  跨函数调用链**（props + vnode + fast-call + 5 次字段解析）。
- 用户指出的两条 vnode 检查结论：
  1. "列创建繁琐固定开销"——证伪：StructNew 是 free-list 复用 + 单块
     分配 + 3 次直写，固定步骤无可省（`JS_NewObjectClass` 只在 miss 时走，
     实测 `hit=1 miss=0` 起步）。
  2. createElement 20B > 12 门限挡内联——维持：门限实测 27 vs 11ms（<83>），
     内联体的 _wv/寄存器体质不变之前调参即亏。
- 真解（与 immutable 同构）：props 字面量内联进 createElement（同一
  vr-inline 框架，`{id,cls}` 在调用点直接 StructNew）或调用点与工厂
  一起做"同 shape 原地写"。记入 vr-inline 转正条件。

## 本轮副产品（已验证行为，无代码改动）
- 真 release（`xmake clean -a` 后全量重建）是静默的：WHALE_* 诊断只进
  `--whale_debug=y`（<62> 契约成立）；`xmake -r` 的"cache compiling"是
  缓存命中式增量，诊断开关的变更必须 `clean -a` 才能生效——之前"release
  仍有打印"即此原因。
- 退出崩溃（immutable/vnode，check 对，exit -1073741819）：runner 口径
  （只解析 WHALE_BENCH 行）无影响；`WHALE_ENTER_COUNT` 下无 WHALE_ENTRIES
  行，deopt 计数 9 次 flag=1 后即切换 reading——与 <62> 的"8 次退役"一致，
  属同一预存问题（疑 FreeRuntime 期），本轮不动。
- vr-inline 拒绝链：createElement `rtype=-1 clen=20` 是 12B 体门限（`len>12`
  早返，`whale_inline_body:1632`），不是 define_field/get_field 能力问题
  （ObjDefineValue + OP_null 分支 <83> 已补）。

## 线性 IR 计划（<67> 路线 B 的具体化，下轮输入）
- 目标：生成器直接做寄存器分配，绕开 TCC vstore/gv 机制（L pin 判死刑后
  nested/immutable/vnode 三个缺口的共同解）。
- 输入面：自产规则子集 + 全函数体已在缓冲（loop 缓冲 + 循环表 + fold 链），
  与 <67> 论证一致，估 800-1500 行。
- 第一刀（按本轮证据排序）：
  1. **调用链融合**（vnode/immutable）：props 字面量在调用点 StructNew +
     工厂内联（vr-inline 门限放开的前提：体质先行）；
  2. **循环内 live-range 分配**（nested）：`_wv` 已是单写点 pin，下一步是
     L5/L6 的循环 carried 分配 + `_ws.v` 链的寄存器化（当前每条语句 spill）；
  3. **字段解析提升**（vnode）：ObjFieldNum 的 fs/fp 缓存提到循环外
     （shape_stable 时零检查，<61> 已有语义基础）。
- 非目标：不动 tccgen 本体（<67> 路线 A 永不）；不调 expr_max（本轮证伪）；
  不重开 L pin（<86> 定案）。

## 状态
绿，已提交 5060148。下一步：immutable/objshape 压到 100ms 线 + 退出崩溃修复。

# <88>: 退出崩溃修复 + objshape escaped 相转正 + fver 语义收紧
SOLVED ×3（崩溃 / escaped / 缓存语义）/ PARTIAL（immutable 未到 100ms 线）

## bug#1：退出崩溃根因 = free-list drain 读错链指针（已修）
- 现象：任何"函数内建对象字面量"（`var st={a:0}` + 循环读写，无需调用、
  无需返回对象）的程序 check 对但 exit -1073741819（gdb：崩在
  `JS_FreeRuntime` +627，`mov -0x4(%rdx),%eax` 读野指针）。
- 根因：plain-object free list 的链指针存 `u.opaque`
  （`free_object:9739`，因 prop 块随 shell 保留），但 `JS_FreeRuntime` 的
  drain 仍按旧约定读 `p->prop`（struct 链的约定）→ 把 prop 块当链走，
  逐个 `js_free_shape/js_free_rt` 释放 prop 块与对象体 → 堆破坏。
  struct 链不受影响（它本来就走 `p->prop`）。
- 修法（quickjs.c 一处）：drain 改读 `p->u.opaque` + 补上 `js_free_rt(p->prop)`
 （prop 块随 shell 保留，drain 时才真正释放）。gdb 反汇编 + 二分 repro
  （`temp/crash_repro*.js` 六组）定位；修后全部 status=0。
- 教训：两条 free list 链指针约定不同（struct 用 prop、plain 用 opaque），
  drain 与 checkout 必须同源。此前 <83>/<86> 记为"预存 FreeRuntime 问题"，
  实为 <79> 引入 plain free list 时的配套遗漏。

## bug#2：跨编译 speculative 状态泄漏（已修）
- `whale_vr_fn[]/vr_expect_slot[]` 是文件级 static，只在首次 resolve 时写入、
  从不重置 → 后编译的函数复用前一个函数的 callee 身份与 guard 下标，
  新体编译进错误的 identity 检查。另 `whale_loc_obj[]/obj_pending/ret_obj_g`
  同样跨编译残留（`whale_ret_obj_g` 一旦置 1 永不回 0 → 后续无对象函数
  也走 struct 路径）。
- 修法：`whale_jit_gen` pass-1 前统一重置（jit.c 一处 +4 行）。`whale_loc_fn/
  slot_fn` 已在 2537 行重置，不动。
- 行为变化：零（之前恰好因编译顺序未触发错答案，但 vnode 的 fn_3 等
  struct 路径选择受其影响）。

## objshape escaped 相：474→112ms（转正，进全量）
- 症状：`escaped()`（`out[i&7]=t`，t 为对象）整函数拒编
 （`gave up at pos=60 op=72 put_array_el, bail=4973`），400k 迭代跑解释器。
- 根因：`t` 是 frame-column STRUCT（无 JSValue 在槽里），而 ArrSetVal 快速路
  要求 `WHALE_IS_SLOT(tc)` → `WHALE_STEP_INSTEAD_OF_BAILING`；step 的
  value-position fence 同样拒非数值 → 双重拒绝。
- 修法：STRUCT 值先物化进共享对象槽（复用 `whale_str_obj_slot` + ObjNew +
 逐列 DefineNum，与 call-argument 的 mat-arg 同模式），再走 ArrSetVal。
 `t` 的两列经 L 列直写已是数值，物化即两次 DefineNum。
- 效果：escaped 相 474→112ms；objshape 全量 596→209ms（repeat=1 三引擎对照），
  25/25 全对 0 mismatch。剩余 gap：两相对象读写仍是逐字段 helper
 （twoShapes 49 vs node 17，见下）。

## fver 语义收紧：put/set_loc 的对象赋值不再 bump（objshape 209→193ms）
- 原语义：`put_loc/set_loc` 的对象槽统一路（ObjSlotCopy + `oi=0`）后无条件
  `_ws.fver++`。`fver` 是**全局**版本号，一 bump 全函数 24 对缓存全部失效，
  下一迭代逐个 re-resolve（ObjShape + ObjFieldPtr + hash 查找）。
- 错误所在：局部变量换名（`st` 从槽 A 换到槽 B）没有改变任何 property
  table——字段解析缓存的有效性只依赖"属性增删"，不依赖"局部变量名"。
  前者才 bump（put_field/define_field/call 仍保留），后者免除。
- 修法：删两处 `fver++`（put_loc 对象臂 + set_loc 对象��），注释写明不变��。
  twoShapes/growsShape/writeOrder 均为"对象不出循环 + 字段固定"形态，直接受益。
- 效果：objshape 213→193ms（全量），escaped 相 128→112ms；immutable 281→279
  （调用链主导，几乎无感，符合预期）；25/25 全对。

## immutable 现状：279ms，100ms 线路径已论证（未动手）
- 编译形态（immutable_src.c 实测）：`step` 循环体 = CallSelfValue（整套
  fast-call：arg 借槽 + 最小帧 + entry + 28 槽位清理）+ ObjFieldNum ×3 +
  fp 直接写回；`setField` 本体 = StructNew + 4×ObjFieldPtr 解析 + `_sdp` 直写。
  vr-inline `rtype=40 clen=52` 因 12B 体门限拒绝（放开验证过：364-423ms 更慢，
  TCC 体质撑不起 52B 体，已回退）。
- 每迭代成本拆分（100 万次迭代）：setField 侧 StructNew×1（含 free-list
  checkout + 5 列重写 + GC 挂链）+ 4 次字段解析；step 侧 3 次字段解析 +
  fast-call 全套。v8 13ms 说明此形状可拆箱到寄存器——我们当前每步都在堆上。
- 100ms 线的两条路（均未动手，本轮只论证）：
  1. **原地更新语义**（用户方向）：`st = setField(st, v)` 且 shape 不变时，
     不建新对象，直接写旧对象列 + 触发 proxy/setter。需引擎语义层支持，
     与 CStruct 列存储天然契合，预期直达 ~30ms 量级；
  2. **调用融合**（线性 IR 第一刀）：props/setField 在调用点 StructNew +
     工厂内联，但前提是体质（_wv/寄存器化）先行，否则重蹈 52B 内联更慢。
- 本轮不动的原因：两者都是语义/架构级改动，需单独一轮。当前 279ms 已是
  "全编译 + 零解释器"的正确形态，check 全对。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch（node/qjs/whalejs 三引擎对照，
`temp/bench_88.json`）；immutable 282、objshape 209、vnode 11、nested 87、
object 4（反超 node 24）、fib 19（反超 node 36）。
## 状态
绿，已提交 5a5d4d3。下一步：immutable 原地更新（用户已确认做）。

# <89>: 原地更新 fusion 完整复盘 + 安全赢落地
SOLVED（复盘+安全赢）/ PARTIAL（immutable 未到 100ms，需守卫内联）

## fusion 两轮证伪（均已回退，树干净）
- **callee-side InPlace**（`JS_WhaleStructInPlace`）：工厂在 callee 跑，
  可复用存储在 caller——callee 永远看不到匹配源，恒走 StructNew 回退，
  白付 slot + 检查。已删。
- **caller-side fusion**（vr-inline OP_object 复用实参列 + 12B 门限放行）：
  inline 成功（`rtype=40 clen=52`），但 check=undefined。根因是 aliasing：
  StructFuse 让新旧对象共享列存储，4 个拷贝列的 get_field 读与随后的
  define_field 写是同一地址——读的"旧值"在写后变"新值"，`st.a` 累加链读到
  被污染的列。改读 field-cache（source slot）后正确性仍坏：fused return
  的 ObjSlotCopy 把结果搬离 fused slot，而 put_loc 写 fused slot——读写
  分家。aliasing 在"读-写同地址 + 返回搬离"两处不可收敛，已回退。
- 教训：fusion 的正确形态不是"共享存储"，而是"守卫内联"——callee 体展开时
  每个 get_field 读 caller 列、每个 define_field 写 caller 列，读-写都经
  field-cache，不共享、不搬离。需单独一轮（inline get_field 认 STRUCT +
  12B 门限对 factory 放行 + return 搬 fused slot）。

## 安全赢：StructNew checkout 数字列免 FreeValueRT
- free-list checkout 的列释放循环对每列调 `JS_FreeValueRT`——数字列
  （int/float64/bool/null/undefined）不拥有引用，白付调用。加 tag 检查，
  仅引用列走 runtime。immutable 279→273ms（-2%），objshape 201ms 持平，
  25/25 全对。小但是零风险。

## immutable 100ms 线设计（本轮结论，下轮输入）
- 实测分解（100 万迭代）：base 263ms；同调用链但 callee 原地改（mutate）
  112ms；手写内联（无调用）whalejs 0-1ms / node 5ms。结论：调用链占大头，
  体质（TCC 语句 spill）占小头——fusion 消掉调用即到 100ms 线内。
- 路径：守卫内联（上节）+ 字段解析进寄存器（fver 已收紧，fs/fp 提循环外）。
  不做语义层原地更新（proxy/setter 触发面太大），不做 callee-side 复用。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch（node/qjs/whalejs 三引擎对照，
`temp/bench_89.json`）；immutable 273、objshape 201、vnode 13、nested 70、
object 4（反超 node 25）、fib 15（反超 node 32）。
## 状态
绿，已提交 b6377af。

# <90>: vr-inline 通用化证伪：12B 门限对 factory 放行反而更慢
SOLVED（证伪）/ 维持现状

## 做的事（前端通用化，按用户方向）
- 用户拍板：对象操作写回的一律内联、原地做、别正常走；核心是前端的事。
- 落点：`whale_inline_body` 的 `len > 12` 门限对 struct-factory 放行
  （`whale_inline_str_n > 0` 即 call site 预扫描出字面量列）。
  一行改动，无其他配套。

## 测量（证伪）
- inline 成功：`[vr-inline] rtype=40 clen=52`，guard 有、call 无，体展开。
- 但 immutable 271→410ms（更慢 51%），objshape 持平 192→194ms，
  vnode 持平。check 全对。
- 展开体：StructNew + 4×field-cache 解析 + `_sdp` 直写 + ObjSlotCopy ×3
  （inline return 的槽搬离 + put_loc 统一）+ loop 体 3 解析。
  52B 体在 TCC 下全走内存，不如 fast-call 的 50ns。
- 结论：12B 门限维持关闭（已回退，树干净）。体质（_wv/寄存器化）不变之前，
  任何 factory 放行都是亏——与 <83> createElement 27 vs 11ms 同一结论。
  通用化内联的前提是体质，不是门限。

## 验证
`whale api test: ok`；25/25 全对，0 mismatch（node/qjs/whalejs 三引擎对照，
`temp/bench_90.json`）；immutable 295、objshape 200、vnode 11、nested 74、
object 4（反超 node 36）、fib 15（反超 node 35）。
## 状态
绿，待提交。下一步：体质（线性 IR / 寄存器化）先行，再谈内联通用化。

# <91>: 真原地 fusion 落地：same-shape factory 在 call site 展开为旧列写
SOLVED（immutable 进 100ms 线）/ PARTIAL（objshape 未动，需另起形状）

## 用户方向（三次纠偏后的定稿）
- 不创建新对象，直接在旧对象上改 + proxy 触发（不走 StructNew 慢路径，
  变成一行的属性修改）。
- 只要对对象操作并写回去的都内联成原地操作；核心是前端的事。

## 做的事（全前端，C++14，无模板无异常）
- `whale_fuse_probe`（`src/whale_jit.c`）：纯类型模拟判 same-shape
  factory——单对象实参喂所有 get_field、n-1 列拷贝 + 1 列数字实参更新、
  每字面量列恰定义一次。`argc != callee->arg_count` 先拒（位置映射前提）；
  `csrc` 全初始化 -1（fresh literal/ICONST 不能冒充实参）；
  copy 列要求 `value == source-fed && fed == srcarg`（旧逻辑"任意数字都算
  拷贝"把更新列也算成拷贝，是第一版 probe 的错）；
  update 列要求 `other-arg && fed == -2`（恰一列）；
  落点要求 source 实参是 caller-owned SLOT、update 实参是 NUM。
- `whale_fuse_emit`：resolve 先、materialize 中、store 后——resolve 自带
  deopt（accessor/缺列/旧值非数字），committed store 不可回滚，顺序即正确性。
  发 guard（VRFN 身份）+ 一对 field-cache resolve/store（update 列写源 slot），
  拷贝列零发射（源列本就对），结果即源类型，无返回搬离。
  `fields_p`/`expect_slot` 显式传参（`whale_jit_gen` 内 `whale_fields` 是
  遮蔽局部，emitter 够不到文件作用域计数器——C 作用域坑）。
- call site（VRFN 路径）：`fm` 提前到 probe 之前（probe/consumes 前截断点一致），
  probe 命中即 emit + break，不再进 `whale_inline_body`。
- proxy/setter 语义：resolve 即触发器——accessor 使 find_own_property 答 NULL，
  ObjFieldPtr deopt；触发只跑在源对象上一次，即语言规定的 store 位置。

## aliasing 为什么这次收敛（<89> 两证伪的对照）
- 不共享存储：无 StructFuse、无新对象，读-写同地址污染不存在。
- 不搬离返回：结果即源 slot，无 ObjSlotCopy 读写分家。
- 不重写拷贝列：拷贝列的 get_field 读与"重写"写是同一列——写了即脏，
  <89> caller-side fusion 的 check=undefined 即此。零发射即正确。

## 测量
- immutable 273→31~33ms（三跑 31/34/33，check=249985 与 node 一致），
  进 100ms 线（优秀线内），约为 node 15ms 的 2 倍——剩余是 loop 体 3 解析
  + field-cache 形态（`_ws.fv/fs/fp`），非调用链。
- objshape 持平 204ms（四相无 same-shape factory 形状，未触发 fuse；
  check=864036481260 一致）。vnode 17、nested 73、object 5、fib 14。
- 25/25 全对，0 mismatch（node 对照逐 case 跑完，check 逐一对过）。
- 生成源码证：loop 体 `VarRefIsFn guard + fv/fs/fp resolve + fp[e]=v`，
  无 StructNew、无 CallSelf/CallValue、无 ObjSlotCopy。

## 验证
`whale api test: ok`；immutable×3 + objshape/vnode/nested/object/fib +
其余 19 case whalejs 跑完 + node 25 case 全对对表。
## 状态
绿，已提交 eb712c8。

# <92>: 固有开销复盘 + fuse_emit 重复提交修复（本轮）
SOLVED（复盘+实修一处）/ PARTIAL（大头未动，需 struct 化）

## 用户方向
- fib 7→22→14 链路说明优化有顾此失彼：找固有开销降，不要再拆东墙补西墙。
- 本轮继续优化。

## 基线（本轮实测，node 对照在后）
- fib 17、immutable 33、objshape 202、vnode 11、nested 75、object 4、
  objfields 68、minmax 103~153（抖动大）。
- node：fib 34（反超）、immutable 13、objshape 46、vnode 2、nested 16~24、
  object 25（反超）、objfields 21、minmax 48。

## 顾此失彼链复盘（<83>/<84> 证据）
- 7→22：`_wv0..7` 无条件声明 + struct 列 32 零初始化 + `_ws.sv[128]` 死存储——
  全是 per-call 税，fib（~240M 调用）全额赔付。
- 22→14：三税清零（门控 _wv/列声明、删 sv）回到 14~16。7ms 地板在 `_ws`
  整体 `{0}`（v/wa/fs/fp/fv/ap/an/ai/op/on/oi/sp 近 3KB，每调用 memset 语义）。
- 教训：任何"所有函数都付"的改动先问 per-call 税；门控条件必须与分配点一致
  （minmax 109→129 回退即声明/分配条件不一致）。

## 固有开销清单（按"谁在付"排）
1. **`_ws` 整块 `{0}`（最大，递归 fib 主税）**：fib core 仅用 v[0..1]，
   却付 68+32+24+24+1+24+28×6+128 ≈ 3KB 零初始化 × 240M 调用。
   拆分（按需清零/分块声明）是大改——<84> 已评估，留待 struct 化专项。
2. **field-cache 形态（loop 内大头）**：每解析 `fv/fver+fs/shape` 双分支 +
   `ObjFieldPtr` 调用 + `fv=fver` 回写。immutable loop 体 4 解析（fuse 写
   1 + 读 3）。stable 位本可消形状检查，但 step 的 loop 体全是 call（含已
   fuse 的 `CallSelfValue`——fuse 只在第二版编译 firing，第一版仍带调用），
   `loop_has_call` 恒真 → stable 恒假。根治 = call-free 循环的解析提升
   （fver/fs/fp 提循环外），需线性 IR 分配面，另起一轮。
3. **entry 税（每编译调用一次）**：CheckStack + unbox + VarRefIsSelf +
   DeoptNote + box，共 5 helper。fib 仅 12 entries（entry 级），可忽略；
   不得为省它动语义（CheckStack 保栈、IsSelf 保双读正确）。
4. **guard/重复发射（本轮实修）**：`whale_fuse_emit` 在外层 `fold_mark` 后又
   对 `fold_obj_field_resolve`（内部自带 mark/commit）包一层 mark/commit——
   resolve 语句在 loop 体提交两次。生成源码 loop 体 `fv/fs/fp resolve ×2`
   为证。修法：直接调 resolve，不再外包。immutable 31~33→27~29ms（-13%），
   objfields 68→59ms（fill 路径同享 resolve，同减一解析）。
   另试过 value-materialize 前置 guard（value→guard→resolve→store），
   31→38ms 反而更慢：materialize 语句把 guard 挤出 LICM 提升位，
   guard 留 loop 内每迭代付一次。已回退——顺序即性能，注释留了证据。
5. **prologue 杂税（已门控，无事可做）**：`_p0..3`（4 指针声明，无初始化，
   零成本）、`_sdp`（1 指针，零成本）、`L` 列（fib 零列；对象函数按需）、
   `_wv`（单循环无调用才 8 个；minmax 嵌套 loop_count=2 不付）、`A`（按需）。
   再砍只能动 `_ws` 本体（即第 1 条）。

## 测量（本轮修后）
- immutable 27~29ms（三跑 27/29/27，check=249985），node 13。
- objfields 59（-13%，同享 resolve 路径），fib 15、objshape 195、
  vnode 11、nested 73、object 4、minmax 121。
- 25/25 全对，0 mismatch（node 25 case 逐一对表）。

## 验证
`whale api test: ok`；19+6 case whalejs 全跑 + node 全对表。
## 状态
绿，已提交 b034520。

# <93>: presence-flag 检查去重（sp[col] 同段只查一次）
SOLVED（objfields loop 体 4 检查→1，零回归）/ PARTIAL（大头仍是 field-cache）

## 落点（读已落盘生成代码定的）
- objfields `run()` 的 loop 体（`temp/cur_of.c`）：4 列布局下每迭代
  `if (!sp[c])` ×4，其中 3 个是读-写同列的重复（写后读、读后读）。
  sp[] 只 0→1（store 置位，无清零；新对象重建布局），同段内已证即恒真。
- 修法：`whale_sp_seen[WHALE_LOOP_MAX]`——每 open loop 一字，位 col 表
  "本段已证"。读（get_field/get_field2 STRUCT 臂）先查字，命中则整段
  `if (!sp[col])` 不发；store（define/put_field STRUCT 臂 + fill 臂）置位。
  loop_head/任何 jump target 处清字（join 后不可沿用一边结论；back edge
  重进 body 亦清——下 trip 不沿用上 trip）。新对象（OP_object 两布局臂）
  全清（旧证属旧对象）。32 位字故 col≥32 不记（布局上限 32，`sp` 数组
  `4×32`，够用；超限即不去重，保守正确）。
- 证伪记录（已回退）：去 store 的 `sp=1` 写——objshape 202→404ms 翻倍。
  原因不是 flag 写本身贵，而是写与读在 TCC 下是 load-store 转发的锚：
  去写后读臂的 `if (!sp[c])` 读到的是 `{0}` 初值语义，TCC 把跨语句的
  `sp[c]` 每次重载，分支预测全崩。写保留、只去重复读，才是正确方向。

## 测量
- objfields loop 体 `if (!sp[c])` 4→1（落盘 `temp/cur_ded.c` 为证：
  仅剩 `flags` 列首次读一次；`tag/key` 写后读全消，`depth` 读后读消）。
- objfields 59ms 持平（check=4799933）：分支 4→1 但体仍是 field-cache
  形态（fill 走 `ObjFieldNum` helper，loop 体走 STRUCT 列+L spill），
  分支只占小头——与 <92> "field-cache 是大头"一致，无矛盾。
- 其余：fib 17、immutable 31、objshape 197、vnode 11、nested 70、
  object 5、minmax 119。25/25 全对，0 mismatch（node 25 case 逐一对表）。

## 验证
`whale api test: ok`；8 关键 + 17 其余 whalejs 全跑 + node 全对表。
## 状态
绿，待提交。下一步：field-cache 形态（call-free 循环解析提升）/ `_ws` 拆分。

# <94>: 纯数值后端优化第一轮（比较-分支融合 + 位运算短式 + 热局部写点物化）
SOLVED（branch/minmax/loop_sum 兑现，零回归）/ PARTIAL（bitops/locals/nested 大头仍是 TCC 语句内 spill）

## 基线（repeat=3，whalejs vs node）
arith 106/64（1.66x）、locals 100/26（3.8x）、nested 71/19（3.7x）、
minmax 123/52（2.4x）、bitops 150/14（10.7x）、loop_sum 72/17（4.2x）、
branch 105/28（3.75x）、floatlit 130/63（2x）。fib 反超中，不在本轮。

## dump 数出来的四处确定热点（WHALE_JIT_SRC 落盘逐条数过）
1. **比较-分支永不融合**：每个循环条件都是 `if ((((a<b)) ? 1 : 0) == 0) goto`——
   先 `?:` 物化布尔 double（TCC 语句边界 spill 一次访存），再归零比较。
   branch 每迭代付 5 次。
2. **ToInt32 过度冗长**：`(int)(long long)(x)` 约 15 token 包住每个位运算操作数；
   已知 int32 的值（`&|^` 结果、字面量）也走全套，链式表达式爆炸 →
   超 24 token 上限 → 每个中间值物化成 `_ws.v` 语句。bitops 体内 14 条
   `_ws.v` 全是这么来的。
3. **`%` 操作数无条件物化**：`fold_mod` 注释自称短操作数可直接内联，但调用前
   两行无条件 `fold_materialize`——`L1`、`3` 这种单 token 也先存成纯拷贝语句。
4. **`!x` 的 NaN 臂对 int32 是死代码**：`((x!=x)||(x==0))?1:0` 提操作数 3 次，
   int32 不可能是 NaN，一句 `(x==0)` 就够。

## 落地的改动（全在 src/whale_jit.c，生成器侧，不动 TCC）
- `fold_toint32(k, t)` 改传类型字节：ICONST 字面量直接发整数；
  I32 发单层 `(int)`（C 的位运算要 int 操作数，`double & double` 不编译；
  全套 `(long long)` 只留给一般 NUM）。5 个调用点全改。
- `OP_mod` 短操作数（`len<=8`）免物化（`v[0]=(L1)` 这类拷贝消失，
  loop_sum 72→58ms）；I32/ICONST 分子 + 正非常量除数走裸 `(int)a%(int)b`
 （C 取余符号随被除数 = JS 语义，除数非零常量故无 UB，fmod 旁路消失）。
- `OP_lnot` 的 I32/ICONST 特化（单比较，不再三提操作数）。
- **比较-分支融合**（通用路径 + peephole-1 非 dst 臂）：`cmp; if` 合成
  `if (!(a<b)) goto` / `if ((a<b)) goto`。`!` 而非反转操作符（NaN 语义：
  `!(NaN<1)` 真，`NaN>=1` 假，原来算前者）。`whale_next_is_branch` helper
  覆盖 8/32 位 if。
- `local_set` 写点热物化：`whale_loop_heat[d]>=3`（pass-1 循环内读+2写计数）
  的局部在写点直接落盘（语句边界，安全），后续读变变量读。
  bitops 的 `v=i&255` 从 4 次全套转换展开变成 1 次计算 + 3 次变量读，
  `_ws.v` 语句 14→4 条。

## 一个真 bug（第一批 lnot 特化引入的，顺手修）
lnot 的 I32 判断读了循环级残留变量 `t`（该 case 从未赋值）——残留碰巧是
I32/ICONST 时走单比较，否则走通用路径；残留对上但操作数是可能 NaN 的
NUM 时静默错答案。改现取 `cur[var_count+sp-1]`。回归全对故未爆炸，
但这是定时炸弹。

## 一个大坑：融合第一版跳过 fold_set，objshape escaped 相编译失败
第一版融合为省 BOOL 槽直接 `sp-=2`，没走 `fold_set`/`cur` 更新 →
escaped 相出 `v[1] = ();` 空物化（TCC 层 `identifier expected`）→
连锁第二次尝试 `duplicate label L8`（单例 state 错误路径清理不彻底，
残留污染下一次）。entries 36（掉一相）、objshape 197→495ms。
修法（方案C）：状态机与原来逐字一致（BOOL 槽照记照消费、两次 sp--、
快照/pos 跳跃），只发射形态是融合的（`if (!(比较))`，BOOL 槽文本已无
`?:`）。lnot 融合则整体删除（I32 特化后它与原来逐字一样，零收益白占复杂度）。
教训：**融合类优化必须镜像原来状态机的每一步（fold_set/cur/sp/快照），
只改发射形态；跳过 bookkeeping 省的那点东西一定在别处加倍还回来。**

## 效果（repeat=3）
- branch 105→52ms（-50%）、minmax 123→81ms（-32%）、loop_sum 72→49ms、
  nested 71→80ms（+13%，无机制解释，判噪声，下轮重测）、其余持平。
- objshape 178ms entries=48（恢复全编译）；25/25 全对 0 mismatch；
  `whale api test: ok`。
- bitops 150→147ms 纹丝不动：`_ws.v` 14→4 条零收益 → 瓶颈不在语句数，
  而在语句内（5 个整数 op + idiv）与分支预测（`v&1` 交替致 `if` 50% 误预测，
  TCC 无 cmov 是架构性差距，tcc_to_optimize.md 第 7 条）。记入下轮（线性 IR）。
- locals/arith/floatlit 体已是最简语句，剩的是 TCC 单语句内 spill，
  同等线性 IR 后端（路线 B）。

## 验证
`whale api test: ok`；25/25 全对 0 mismatch（node 逐一对表）；
repeat=3 关键 case + repeat=1 全量。
## 状态
绿，待提交。下一步：线性 IR 后端（bitops/locals/nested 的共同解，
<67> 路线 B）；nested +13% 重测；`_`ws` 拆分（fib per-call 税）。

# <95>: ELS/ISA 现状审计 + 小 trip 循环展开（minmax 追平 node）
SOLVED（minmax 81→49ms = node）/ TIPS（FMA/cmov/AVX/roundsd 全部证伪）

## ELS + ISA 现状（动手前先审计，避免重复造）
- TCC 侧 pin 机制 8 处全在树上（`whale_pin_*` + modrm 识别 + `whale_isa_*`），
  AVX2 VEX 分支在 `gen_opf`（2244 行），CPUID + `WHALE_ISA` 覆盖全就位。
- 生成器侧只剩 `_wv` 2-pin（L pin <86> 已判死刑）。
- 但 ISA 分级是空转的（<45> 早写过）：产物只有 `+-*/` 比较转换，无
  `roundsd` 目标；AVX 三操作数因 `dst==src1` 恒成立零收益。

## 四个候选的证伪结论（每个都有具体理由，不是"以后再说"）
- **FMA 死刑（语义）**：`fma(a,b,c)` 单舍入 vs JS 双舍入，check 精确比对必
  mismatch。除非运行时证明 `a*b` 精确，不可编译期证明。V8 敢用是有守卫，
  我们没有。
- **cmov 无落点**：TCC `expr_cond` 是跳转实现（gvtst+gjmp+gsym），双分支归并
  处改 cmov 要动寄存器归并，单遍下无低风险点。生成器侧发 `?:` 同样是分支。
- **AVX/roundsd 无目标**：见上。`roundsd` 要等 `Math.*` 进 JIT。
- **跳转缩短高风险**：`gsym_addr` 长跳转回填改短要压缩代码 + 修正已回填地址，
  单遍下风险大收益小（icache）。跳过。
- **PE callee-save 无事可做**：PE 版 prolog 只 `push rbp/mov/sub`，根本不存
  非易失寄存器（tcc_to_optimize.md 第 10 条在 PE 版不存在）。

## 落地的：小 trip-count 循环展开（生成器侧 loop buffer）
- `whale_loop_trip_of`（pass-1 后）：只认 `g=0; g<K; g++`（K≤8）+ 单一 inc 写
  + 无调用 + 无外跳（head test exit / 回边除外）+ 无内层循环头。跳转目标
  公式照 pass-1（`pos+1+offset`，第一次写成 `+2` 全拒，定位手段是 `[trip]`
  打印）。
- `whale_loop_unroll`（`loop_end` 处）：头 flush 留一次、删头标签/条件/回边、
  体复制 K 份（`Lg` 读换字面量、`Lg=...` 整句删、内部 label 逐份重命名
  `Q<id>_<k>`、`goto exit` 出现即拒）。iface 加 `JS_WhaleTokIsKw` +
  `JS_WhaleTokCInt` 两个 5 行导出。
- 附带修两个真 bug：① `loop_end` 触发条件 `pos == back_pos` 永不成立
  （`pos += sz` 在检查前，2 字节 goto 让 pos 超过 back 2）→ 改 `pos - sz`，
  原来所有 buffer 全开到函数尾（LICM 不依赖它故无人发现）；② 该修复让
  嵌套循环外层余部 depth 回到 1，撞上 `_wv` pin 门与声明门不一致
  （materialize 缺 `loop_count==1`）→ 补上，否则嵌套函数发未声明 `_wv`
  直接编译失败。教训：门控条件必须与声明点逐字一致（<83> minmax 回退同款）。
- 效果：minmax 81→**49ms（追平 node 48ms）**，`j*7` 全变字面量（TCC 常量折叠
  免费吃掉），8 份标签重命名验证正确，25/25 全对。
- bitops 全量口径 234ms 是机器负载（单测 repeat=3 回 150ms），非回归。

## 验证
`whale api test: ok`；25/25 全对 0 mismatch；minmax repeat=3 追平 node。
## 状态
绿，待提交。下一步：SIMD（sieve，生成器侧循环识别 + 步长展开，最大单项）；
`_ws` 拆分；线性 IR（bitops/locals/nested 共同解）。

# <96>: 类型统一地基确认 + Uint8Array fill 循环整成 memset（sieve -24%）
SOLVED（sieve 144→111ms，25/25 全绿）/ TIPS（类型保证清单）

## 用户提醒的确认：类型统一是分版本 + 守卫保证的
- 参数入口 `whale_jit_unbox_num` 守卫（非数值 → deopt 整帧重跑）；
  闭包 self/closure 双读法 + 入口 `VarRefIsSelf/IsFn` 守卫（deopt==3 换版本）；
  跳转目标类型快照合并（差异 → 整函数拒编，绝不混类型）；
  对象走独立槽/类型，NUM 通道恒 double。
- 结论：**纯数值函数体内 NUM 槽恒 double，循环变量类型不变** —— 向量化
  不用逐元素 guard（入口 + length guard 足够），寄存器分配不用处理类型
  变化。这正是本轮 memset 与下轮 SIMD 的地基。

## 落地的：`for (g=C0; g<=K; g++) arr[g]=C` → 单次 memset
- `whale_loop_memset_of`（字节码骨架识别）：`g=C0` 紧邻头 + `g<=K/<K`
  （K 字面量**或参数**，sieve 界是参数 n）+ `g++` 回边 + 体内恰好
  两个 get_loc（一读 g、一读容器）+ 一个 0..255 字面量 push + 一个 put
  + 其余只许栈洗牌（`arr[i]=v` 编译带 swap/dup/to_propkey/undefined 检查，
  第一版"紧邻三连"假设直接撞墙）+ 无调用无内层头。
- `whale_loop_memset_emit`（`loop_end` 处，TAU8 身份由 body token 确认：
  恰好一次 `TAU8Ptr` resolve + 一次 `_ws.op[os][..]` 双层下标存储，同槽，
  其他 `ident (` 全拒）：resolve 照抄体协议 + length guard
  （`(K+1)>len` deopt，K 参数时读 `An[]`）+ 非正长度跳过存储（零 trip
  语义）+ `memset(op+C0, C, K-C0+1)` + 归纳终值。
- `memset` 经 `WHALE_HOST_SYM` 接 libc + preamble 自有原型（`void*,
  int, unsigned long long`，防 C89 隐式声明陷阱，同 fmod 教训）。
- 三个定位花的坑：① 跳转目标公式照 pass-1（`pos+1+offset`，沿用 <95>
  教训）；② store 模式必须双层下标（单层会把 resolve 自身数进去，
  stores=2）；③ foreign-call 检查误伤 `!(`（操作符<256 直接放行，
  关键字 28 个逐一排除，剩下的才是真 ident 调用）。
- 附带修：trip/memset 三表 `memset(ptr, 0, sizeof(ptr))` 只清 8 字节
  （堆数组，要 `(len+1)*sizeof(int)`）—— 无声 bug，sieve 碰巧没炸。
- 效果：sieve 初始化循环（30 万迭代）→ 1 resolve + 1 guard + 1 memset，
  **144→111ms（-24%）**，check 对，25/25 全绿。内层步长循环不动
  （scatter 无解）、计数归约不动（向量化复杂），记下轮。
- bitops 全量口径 234ms 是负载（repeat=3 回 150ms）。

## 验证
`whale api test: ok`；25/25 全对 0 mismatch；sieve repeat=3 77ms。
## 状态
绿，已提交 bc4fb15。下一步见 <98>。

# <98>: 版本号解耦 + `_ws` 拆分（fib 12→3ms 反超 node 10 倍）
SOLVED ×2（版本 0.1.0 落地；fib per-call 税清零）

## 版本号（用户要求，与 QuickJS-ng 解耦）
- `whalejs.h` 加 `WHALE_VERSION_MAJOR/MINOR/PATCH`（0/1/0）+
  `JS_WhaleGetVersion()`（quickjs.c 实现，自有 `STRINGIFY`，不动上游宏）；
  `qjs.c --version` 改双版本（`WhaleJS 0.1.0 (QuickJS-ng 0.16.2)`）；
  `xmake.lua set_version("0.1.0")`；`CHANGELOG.md` 起头。
- 规则：MAJOR=接口/ABI 不兼容，MINOR=新优化/新覆盖，PATCH=修 bug；
  上游升级只合代码，不跟号。

## `_ws` 拆分：`_wsv` 裸数组 + `_wsc` 条件组
- 现状：`_ws` 整块 `{0}` 约 3KB（v/wa/fs/fp/fver/fv/ap/an/ai/op/on/oi/sp），
  fib 240M 调用 × 3KB = 720GB 内存写，是 12ms 的大头。
- 第一步：`wt_arr` 本体分流（`v` → `_wsv[n]`，其他 → `_wsc.<f>[n]`）+
  prologue 发 `double _wsv[N];`（**不零初始化**，槽先写后读，零初始化纯税）+
  其余保持 `_wsc = {0}`。8 处手写 `wt_id("_ws")`（fver/wa）全改 `_wsc`；
  memset/count 的 body 扫描同步认 `_wsc.op`（否则整循环优化永不触发）。
- 第二步：`_wsc` 按组条件声明 —— field 组（wa/fs/fp/fver/fv/sp，
  `need_cols` 复用）+ shared `oi`（对象槽复用清零位 + TAU8 flag，两边都要）
  + array 组（ap/an/ai/op/on，`whale_has_array` 新标记，pass-1 见
  array_el 即置位）。三组全无 → struct 声明整体消失（fib 只剩 `_wsv`）。
- 两个坑：① `ap/an/ai` 初划给 call 组（名误导，实为 packed 数组缓存；
  `oi` 初划给 array 组（实为槽复用协议，对象函数也清它）→ `field not
  found: oi/ai` 连崩对象/array 全族。教训：字段归属看**写它的代码**，
  不看名字。② `struct {} _wsc = {0}` 空 struct 非法（三组全假的 fib
  直接 `too many field init`）→ 外层门控。③ `put_loc8` 紧邻检查三处全
  写反（读 `pc[head]` 实为 `pc[head-2]` 读 `pc[head-1]`）—— `k`=局部 5
  才暴露（同 <97> 教训）。
- 未解之谜（记录）：pass-1 主循环开头的直接 `if (op == ...)` 置位 work，
  同一条件的链尾 `else if` 永不跑 —— 链中间必有吃掉它的分支但没找到，
  直接 `if` 更简单，保留现状。
- 效果：**fib 12→3ms（反超 node 32ms 近 11 倍）**，sieve 62/minmax 48/
  objshape 178 全保持，25/25 全绿。

## 验证
`whale api test: ok`；25/25 全对 0 mismatch（repeat=1 全量 + repeat=3
关键 case：fib 3、sieve 62、minmax 48、objshape 178）。
## 状态
绿，待提交。下一步：线性 IR（bitops/locals/nested 共同解）；SIMD 等
数组面扩大后再做。

# <97>: 计数归约整成单次 helper 调用（sieve 累计 -47%）
SOLVED（sieve 111→77ms，累计 144→77ms，node 42ms 到 1.8x）

## 落地的：`for (g=C0; g<=K; g++) if (arr[g]) acc++` → 单次 CountNZ
- 引擎侧 `JS_WhaleTAU8CountNZ(ctx, slot, lo, hi)`（quickjs.c，照 `TAU8Ptr`
  的槽/类检查模板，-1 = deopt）：`[lo,hi]` 非零 byte 计数。普通 C 循环，
  gcc -O2 自带向量化（`psadbw` 自动出来）—— 向量化在 C 侧免费实现，
  生成器侧只做循环消除。这正是类型统一地基的用法：无需逐元素 guard。
- `whale_loop_count_of`（字节码识别）：骨架同 memset（`g=C0` + 界字面量/
  参数 + `g++` 回边 + 无调用无内层头），体要求 `get_array_el` 一次
  （`get_loc arr; get_loc g` 紧邻三形态回找，读路径无洗牌）+ `if_false`
  inner join（零跳过）+ `inc_loc acc`（acc≠g）+ 其余纯数值。`dec` 不认
  （语义是 `-popcount`，别混）。
- `whale_loop_count_emit`（`loop_end` 处，TAU8 确认同 memset：一次 resolve
  + 一次 `op[os][` 读，同槽）：`acc` 先 materialize（pending 增量落盘，
  否则丢增量）再 `acc = acc + CountNZ(...)` + `g` 终值 + length/非正 guard
  （同 memset）。HOST_SYM + 自有原型（double 返回，防隐式声明取整陷阱）。
- 两个坑：① `get_array_el` 不在 `whale_is_pure_num_op` 表里 → 体扫描先拒，
  加例外（TAU8 身份由 emit 确认，扫描只放行）；② 三处 `g=C0` 紧邻检查的
  `put_loc8` 形态全写反（`pc[head-1]==OP_put_loc8` 读 `pc[head]`，实为
  `pc[head-2]` 读 `pc[head-1]`）—— `k` 是局部 5（put_loc8，2B）才暴露，
  `i/j`（put_loc0..3）全碰巧通过。教训：多字节 opcode 的"前一条"有两种
  宽度，测试只覆盖 1B 形态等于没测。
- 效果：计数循环（30 万迭代）→ 1 resolve + 1 guard + 1 调用，
  **sieve 111→77ms（本轮 -31%，累计 144→77ms -47%）**，node 42ms 到 1.8x，
  check 对，25/25 全绿。剩余 sieve 时间在筛循环（步长 scatter，无解）。

## 验证
`whale api test: ok`；25/25 全对 0 mismatch；sieve repeat=3 77ms。
## 状态
绿，已提交 bc4fb15。下一步见 <99>。

# <99>: IR 不要了 + re-JIT 两级骨架（tier1 常驻底线，tier2 空壳待填）
SOLVED（骨架零变化验证）/ TIPS（IR 证伪 + V8 三级映射 + 侵入红线）

## IR 证伪（用户方向，记录结论）
- 已落地的优化没有一个需要完整 IR（融合=单步前瞻、折叠=2-3 条窗口、
  LICM=输出指针切换、unroll/memset=字节码表+已有 buffer、heat=计数器）。
  需要前瞻的两处（跳转合并 snap、循环改写 loop buffer）早就是按需缓存。
- 结论：**永远不建全函数 IR**；要前瞻用字节码级摘要表 + 已有 buffer；
  TCC 侧保持单遍流式。token→IR 翻译器已删（`whale_emit_x64.c` 降格为
  发码小件库，只留探针 + 字节缓冲/commit；测试回退只验探针）。
- 映射 V8：解释器=Sparkplug 位，TCC 单趟 + 生成器优化=Maglev 位
  （Maglev 开销，TurboFan 效果：minmax 追平、fib 反超 11 倍），直发/x64
  = TurboFan 位（只给热点，复用 token 流）。
- 编译开销红线：unroll 体上限（2048）+ token 总量上限（已有 32768）+
  memset 字面量界上限（1G）；超预算一律原样发射（bail 回正常路径）。
  广告性编译（为编而编）永不进 tier1。

## re-JIT 骨架（用户方向：TCC 可大改，分级有效，tier1 绝不白编）
- 语义：tier1（~1ms 单遍）常驻底线（deopt<8 重武装，8 次退役，**不动**）；
  tier2（整函数缓冲/分配/向量化，以后按"开销大又不通用"一条一条加）
  只给跑得久的函数。tier2 失败 → 回 tier1（不断崖）；tier2 deopt →
  换回 parked 的 tier1（不记 tier1 的账）。
- 落点（全 quickjs.c，生成器零改，解释器 opcode switch 一行不碰）：
  Bytecode 加 5 字段（tier/tried2/tier2_count/entry_tier1/tcc_tier1，
  零初始化现成）；`whale_on_call` 加 tier2 触发分支（tier==1 +
  tier2_count 过 `10×` 阈值 + 未试 → `whale_rejit_tier2`，现恒 bail）；
  tier1 成功处置 `tier=1`；deopt 两处（CallInternal + fast_call）加
  tier2 退级分支（换回 parked entry/tcc，tier=1）。
- 门：`whale_tier2_enable = 0`（默认关，启用方式后面研究）+
  `whale_tier2_threshold()` 单点（现 `10×` tier1 阈值）+ tier2 handle
  生命周期（成功 park tier1 / 失败当场 release，零新增泄漏；单例 state
  累积老账不恶化）。
- 附带发现：`whale_jit_tcc` 从无释放（无 `tcc_delete`，<8> 老账至今），
  记 <71> 技术债第 1 条，不在本轮还。
- 验证：tier2 默认关 + 空壳恒 bail → fib 5/sieve 64/minmax 49/bitops 153
  全基线零变化，API 绿（含 `[x64] direct emit` 探针），25/25 全绿。

## 验证
`whale api test: ok`；25/25 全对 0 mismatch（repeat=3 关键 case）。
## 状态
绿，待提交。下一步：tier2 内容第一条（L pin 复活 = ELS 最小侵入落地，
修 <85> vstore/gv 不对称洞）或 `lea` 熔合进 `gen_opi`（单遍零缓存）。

# <100>: L pin 复活进 tier1（write-through 修好 <85> 不对称洞）
SOLVED（arith -35%、locals -19%、floatlit 反超 node，零 hang 零错答案）

## 用户方向确认（一套发码 + 通用低开销全放 tier1）
- 独立 x64 后端路线作废（同样优化写两遍，双份维护）。`whale_emit_x64.c`
  留探针 + 字节缓冲/commit 作发码小件库，VEX/REX 编码以后在这里孵化、
  成熟一条搬一条进 `gen_op*`（熔合，不是重写）。
- tier2 只收"单遍做不了、要整函数缓冲/多遍"的（以后按"开销大又不通用"
  一条条加）；开销小 + 通用的（pin/融合/展开）全在 tier1 做完。

## <85> 不对称洞的精确形态（读代码定案，不是猜测）
- vstore 捷径只写 pin 寄存器（home slot 不同步）；`save_reg_upstack`
  把 pin 寄存器 spill 到 TCC **临时槽**并改写 vstack 条目（home→temp），
  但 pin 表还按 home 登记、位也不清 → 后续 home 读认领已被重用的寄存器，
  读到垃圾（hang.asm 的 `i++` 消失 + 比较错乱即此）。
- 修法 = **write-through**：vstore 同时写寄存器 + home slot（寄存器变读
  缓存）；spill 则清位（home 新鲜，下次读重载）；真调用清全位
  （Invalidate 的 writeback 循环删除，home 本来就新鲜；有调用的函数
  本来就不 pin，删了零风险）。
- 性能账：写 +1 store（与无 pin 持平），读 -1 load；循环内读>>写，净赢。
  三处全 tccgen.c（vstore/store 回写 + spill 清位 + Invalidate 简化），
  gen_op 归一化与 gv 捷径原样保留（write-through 下更安全）。

## 生成器侧 L pin 恢复（heat top-2，单循环无调用门现成）
- prologue 的 L 声明循环加 top-2 `register`（对象/lo_c 单赋值排除，
  heat<3 不 pin）。L 先声明先拿 pool（预算 2），`_wv` 拿不到则普通变量
  （安全降级，实测无 measurable 损失）。
- 效果（repeat=3）：arith 105→68（-35%）、locals 100→81（-19%）、
  floatlit 106→63（反超 node 66）、minmax 保持追平 48、nested/bitops/
  fib 持平；25/25 全对，零 hang。`_wv` 降级被 L 收益覆盖（nested 持平为证）。

## 验证
`whale api test: ok`（含探针）；25/25 全对 0 mismatch；repeat=1 全量 +
repeat=3 关键 case。
## 状态
绿，已提交 ebdf6c3。下一步见 <101>。

# <101>: `lea` 熔合进 `gen_opi` + 多方案结合的门控策略
SOLVED（零回归，AST 中间替代第一实例）/ TIPS（无条件赢 vs 开分支的两类划分）

## `lea`（用户点名先做，工作量最小）
- 落点 `gen_opi` 的 `*` 分支（1912 行，原来无常量特判直接 `gv2+imul`）：
  右操作数常量（`cc` 现成）+ `imm ∈ {3,5,9}`（`2^k+1`）→
  `lea r32, [r32 + r32*k]`（mod=00 + SIB，`orex` 打 REX）。只做 32 位 int
  （`!ll` 门，64 位编码等验证后开）；符号无关（地址算术，低位与 imul
  一致，回绕语义一致）；不碰 flags（TCC 算术结果无 flag 依赖）。
- 单遍零缓存（常量当场可见），与 `+` 的立即数路径（1865 行）同构。
- 效果：minmax 的 `i*3` 每外迭代省 1 imul（200 万次，~1ms，被噪声淹没
  但零回归）；25/25 全绿，API 绿。`j*7` 做不了（7≠2^k+1，需两条，
  不划算）。

## 多方案结合（用户要求：分支型防负优化，混合场景不受害）
- 划分（本轮定稿，以后照此执行）：
  - **无条件赢**（无分支、无膨胀、无额外访存）：融合/短式/`lea`/ToInt32
    短路 → **无门，直接上**。`lea` 恒 1 cycle vs imul 3 cycle，code 更小，
    不可能负优化。
  - **开分支/膨胀/访存的**：unroll（分支消失但代码 ×K，icache 压力）→
    体上限 2048 + trip≤8 双门；memset/count（分支消失，整循环消除，无
    新增分支）→ trip 有界 + guard；pin（访存换寄存器，pool 竞争）→
    heat 3 + 单循环无调用 + pool 2；guard 类（`?:` 守卫）→ 守卫免费
    实测（idiv 26 cycle vs guard ~2）才上。
- 混合场景（React render 这类数值+对象+调用混用）：数值段吃无条件优化
  （恒赢），对象段走 CStruct/fusion（<91> 已落地），调用段走 fast-call；
  开分支的优化只在**纯数值循环**开（`loop_count==1 && !has_call` 门现成），
  混合函数自动豁免 —— 门控条件即混合保护，不需额外开关。
- 教训：门控条件必须写在**决策点**（不是事后开关），且与声明点逐字一致
  （<83>/<100> 两次吃过亏）。

## 验证
`whale api test: ok`；25/25 全对 0 mismatch（repeat=1 全量）。
## 状态
绿，已提交 7759556。下一步：bitops 主链（`_wv` 已 pin，但类型合并把 acc 降成 NUM，见 <102>）；
`_ws` 剩余（`wa` 布局数组？已够小，不动）。

# <102>: bitops 两处生成器修复（heat 短式豁免 + 位运算进 purity 表）
SOLVED（两处都是"读 trace/dump 定案"，无猜测）

## 根因（WHALE_MEM_TRACE + dump 定量）
- 猜测是"acc 自引用链膨胀"（deferred local 反复 inline），trace 证伪：
  L0 每次 `put_loc` 后都因 `heat>=3` **即时物化**（`[set]→[mat]` 紧邻，
  pending 从未存活）。真正的浪费是 heat 分支**不问长短**：pending 只是
  4 token 的 `(_wsv[n])` 纯拷贝，物化凭空多出 `L0 = (_wsv[n])` 存储，
  每 trip 3 个（dump cur_bitops6.c 94/95/97 行）。
- 第二处：dump 里**一个 `_wv` 都没有**。`fold_materialize` 的 pin 门
  （`!has_call && depth==1 && loop_count==1`）全满足，但 `has_call` 被
  误置：`whale_is_pure_num_op` 没收 `and/or/xor/not/shl/shr/sar/mod`，
  位运算循环整循环标 has_call → ELS 的 `_wv` pin + L-pin 全灭。
  注释写"conversions that need ToInt32/fmod"，但现在这些 op 走的是
  inline cast + guarded `%`，循环体内**无 helper call**（`v%7` 是 I32
  短路 bare `%`），pin 穿过无正确性问题。

## 改动（全 src/whale_jit.c 生成器侧，不动 TCC）
1. `local_set` heat 分支加 `m->expr.len > 8` 豁免：短 pending 保持
   deferred（后续读 inline 同值，无额外访存），长链仍物化，上限 24
   不动。8 = 纯拷贝 `(_wsv[n])` 4 token 的 2 倍余量。
2. purity 表加 `OP_mod/OP_not/OP_lnot/OP_and/OP_or/OP_xor/
   OP_shl/OP_shr/OP_sar`：位运算循环不再误标 has_call，`_wv` pin
   与 L-pin 按原门控恢复。门控策略（<101>）不变：这是"无调用"事实
   修正，不是新开分支。

## dump 对比（cur_bitops6.c → cur_bitops8.c）
- 3 个 `L0 = (_wsv[n])` 纯拷贝存储消失（剩 1 个分支必需的）；
- 主链 `_wsv[0..2]` → `_wv0..2`（pin 回来了），`_wsv` 只剩声明；
- 剩余：acc 在循环头被 I32→NUM 合并降级（<94> 为拒编修的），后续
  位运算全走 `(int)(long long)` 长链（I32 短式 `(int)` 用不上）。
  下一步即此：合并改"升级 giữ I32"（NUM∩I32→I32），`!` 的 NaN 已在
  lnot 发射处处理，守卫语义不变。

## 测量
- bitops repeat=3：whalejs 142ms vs node 16ms（改前 148-154，同噪声带；
  本轮是**正确性修复**——pin 门误关，访存回到应有位置，计时持平符合预期）。
- 全量 repeat=1：25/25 全对 0 mismatch；纯数值 repeat=3：
  arith 63 vs 68、floatlit 62 vs 64、minmax 52 vs 48（持平/反超），
  locals 85 vs 27、loop_sum 44 vs 18（2-3x，TCC spill，待线性 IR）。
- bench_102.json 落盘（6 纯数值用例 repeat=3）。

## 验证
`whale api test: ok`；全量 + repeat3 check 全一致。
## 状态
绿，已提交 fac7e9b。下一步见 <103>。

# <103>: heat 豁免的 ident 退化 + 自引用门（还解锁了恒等折叠）
SOLVED（分支型优化必须有门，<101> 策略的实例）/ 证伪×1（I32 合并升级）

## 证伪：I32 合并升级做不了
- 想法：循环头 NUM∩I32 合并时 giữ I32，让后续位运算走 `(int)` 短式。
- 不 sound：子分支（如 `if` 内 `acc = acc - x`）可产任意 double，
  升级后后续 `(int)` 短 cast 静默截断小数/越界 = **错答案**，不是慢。
  降级（→NUM）是唯一安全方向。bitops 的 `(int)(long long)` 链是
  double 域统一表示的固定税，消它要 int/double 双轨栈值 = tier2 活。
- 变体实验定量（node 先行，排除负载噪声后 repeat=3）：
  去 idiv（`%7`→`&7`）node 15→12ms，去分支 15→14ms；
  whalejs 三变体 144~164ms 纹丝不动。
  结论：idiv/分支共 ~4ms，剩余 ~130ms 全是转换链 + TCC 语句级 spill。
  bitops 到 tier1 天花板（qjs 530 的 3.7x；node 15 的 9.5x 差距是表示税）。

## 退化：<102> 的 heat 豁免把 ident 从 86 干到 120ms+
- 全量 repeat=1 抓到：ident 118ms（bench_90 基线 86ms）。
- 因果对照：豁免阈值临时改 1000000（=关掉）→ ident 53ms。确认是豁免引入。
- 机制：`L0 = L0` 自存（parser 已把 `s*1` 折成 plain copy）deferred 后，
  后续读 inline `(L0)` 而非变量。TCC 黑盒行为，不深究（语句数一样，
  慢 40%，pin/分配层面的差异）。

## 修法：自引用门（local_self_ref）
- 豁免只给**不含 `L<d>` 的短 pending**（`_wv[n]` 拷贝）；
  pending 含自身变量的一律即时物化（= <102> 前行为）。
- 实现：`gw_tok` 同 TU 直接扫 `[off, off+len)` 找 `"L<d>"` ident id
 （`JS_WhaleTokIdent` 有 2251 行先例；`gw_tok` 直读，无新 accessor）。
- 意外收获：`L0=L0` 即时物化后 `live=0`，peephole-1 的 `load_analyze`
  不再因 live 拒绝（844 行），`/1` 与 `-0` 的代数恒等折叠生效：
  ident 循环体 6 语句→4 语句（`L0=L0` + `_wv0` + `L0=(_wv0)` + 计数），
  ident 86→**54ms**（node 8ms；之前从未进过 60ms 内）。
- bitops 保持 144ms，dump 确认 `_wv` 主链与单存储不变。

## 验证
`whale api test: ok`；全量 25/25（qjs vs whalejs，check 全一致，无 mismatch）；
ident 54×3、bitops 144。node 列缺（`node1.exe` 改名，perftest 认 `node`；
qjs 列即解释器基线，结论不受影响）。
## 状态
绿，已提交 0a16999。下一步见 <104>。

# <104>: int-region 落地（循环整数子区：int 镜像 + 入口 guard + 出口装箱）
SOLVED（单方案，无切换无重编译，编译期一次决定）/ 真 bug ×2（`++` token + peephole 旁路）

## 用户方向（转化问题，不是绕路）
- bitops 的主因确认是 int/double 域错位：每 trip 约 8 次域穿越
 （变体实验证明 idiv/分支共 4ms，剩余 130ms 全是穿越 + spill）。
- 做 int-region：识别循环体内"必须 double"的污染点，没有污染的
  局部变量跑 C `int` 镜像，循环内零转换，出口一次性装箱。
- for 判断是关键：`g=0` + `inc_loc` + `g < K` + 无调用无内外出，
  bound 字面量 ≤2^30 免 guard，参数 bound 入口一次 guard
 （`K > 2^30 → deopt` 整函数退解释器，循环外一次，免费）。

## 落地（全 src/whale_jit.c，单发射管线）
- 识别 `whale_loop_int_of`（仿 trip_of 结构）：back-edge goto、
  back-2 inc、head `get_loc g; X; lt; if_false exit`（`<` only）、
  `g=0` 紧邻 head 前；体内 g 仅 back-2 一写；镜像候选 = int 位运算
  生产的 `put_loc v`（`set_loc/add_loc` 进黑名单不杀整循环，bitops
  的 acc 就是 set 存的）；v/g 的每个读点 triple 检查
 （单目 not 紧邻 / 双目 [load, binop]，binop ∈ and/or/xor/shl/sar/mod，
  `>>>` 与 `+ - * /` 比较一律出局）。
- 发射 6 处：prologue `int I<d>`（前缀 I 确认空闲）+ 入口 guard
  （pos==0 一次）+ get 镜像读（循环内 I，类型 I32）+ inc `I=I+1`
  + put `(int)` 包存（emitter 零改）+ materialize 存 I
  + exit 精确装箱（`pos == exitp`，体内 join 不碰）。
- soundness 边界：`& | ^ ~ << >>` 留 int 安全（JS 定值 int32）；
  `>>>` 排除（超 int32）；`+ - *` 留 double（C int 溢出 UB）；
  `I++` 溢出由 guard 封顶。

## 两个真 bug（全是实证定位）
1. **`++` token 不存在**：`wt('+'); wt('+')` 发成两个独立 `+`，
   TCC 解析成二元加 + 一元加 → `expected expression before ';'`，
   连带 `duplicate label`（解析错位）。修法 `I = I + 1`
   （加新 operator id 不值得）。教训：token 流没有"相邻即组合"，
   `<<` 之类走 wt_op 就是因为这个。
2. **peephole 旁路镜像**：head 测试走 peephole-1（load_analyze +
   load_emit 直发 `L2`），不经 get_loc 通用路径 → 测试读 L、回边
   步进 I → 无限循环（400×fibi hang，单次调用因不 JIT 而正常，
   极具迷惑性）。修法：load_analyze 对循环内镜像拒绝 WLD_SLOT
   （退回通用路径）+ local_inline 非 live 镜像发 I（ covering
   get_loc0_loc1 双读）。教训：读路径有三条（peephole/通用/双读），
   加新表示必须三条全堵。

## 测量（repeat=3）
- bitops 144ms 持平：dump 确认镜像生效（`I1<A0`、`I1=I1+1`、
  `I2=(int)...`、出口装箱），但访存结构没变（7 语句、_wv、acc 的
  double 家）——转换 ALU 只占小头，spill 是主体。**前端转换优化
  到墙**，剩下的要么语句合并（线性 IR，后端活），要么接受。
- 意外两胜：branch 66→42ms，nested 71→20ms（3.5x，嵌套内层命中）。
- 全量 25/25 check 全一致；`whale api test: ok`（TCC + gcc 双路径）。

## 状态
绿，已提交 87cbda5。下一步见 <105>。

# <105>: IBIT 裸核落地（bitops 144→128ms，-11%）
SOLVED（单管线：新类型 + 生产/消费两分支，无新管线无切换）

## 打法（<104> 定的两步，第一步）
- 现状对齐：TCC 语句级 spill 架构性无小改（<82>）；ELS 安全部分已拿完
 （_wv 2-pin + L-pin write-through）；bitops 130ms 主体是 7 语句×spill。
- 语句合并（87 行 use-count + 总长门）ROI 重估后放弃：IBIT 把链内
  噪音消完后，可合的只剩 2 对 store/load（约 5ms），实现要动上限
  体系（<82> 证伪过无脑放），风险/收益不对。
- IBIT（WHALE_T_IBIT=12，纯编译期 hint）：位运算双操作数 int-clean
  时生产裸 int（无 `(double)` 包），消费零转换（fold_toint32 直接
  inline）；NUM 操作数走老路（`(long long)` 语义负载）；`>>>` 永不；
  merge 降 NUM（两 arms + type_is_num，三处缺一即拒编）；
  `+ - * /` 比较 return put 跨块全无需改（C 提升/隐转语义自洽）；
  `%` bare 短路 + lnot 单比较补 IBIT（漏网各一）。
- int-region get/put 镜像类型 I32→IBIT（裸 int 更精确，消费全零转换）。

## dump 对比（cur_bitops11 → cur_bitops13）
- `^` 右：`(double)((int)(I2)<<3)` → `((I2)<<(3&31))` 裸；
- `|` 双 int-clean → 裸生产，直接 `L0=(...)`（`_wv` 落地消失一级）；
- `~`：`((double)~(int)(L2))` → `((~(I2))&15)` 裸；
- lnot：`((I2)&1)` 三份 NaN 展开 → 单比较（`==0?1:0` 一份）；
- 语句 7→6（`_wv3/4` 独立落地消失）。
- 剩余转换全在 acc 的 double 家（NUM，老路，认了）与 `+`/`&0xffff`
  的边界（语义必需）。

## 测量（repeat=3）
- bitops 144→**128~131**（-11%）；branch 42→39，nested 20→19（保持）；
  ident 55（61 是单次噪声，repeat=3 回到线）。
- 全量 25/25 check 全一致；API 双路径绿。
- 到 node 15ms 还有 8.5x：剩余全是语句级 spill（线性 IR 的活，
  800-1500 行，见 <87>；本轮证明前端转换税已基本收净）。

## 状态
绿，已提交 452b95e。下一步见 <106>。

# <106>: objshape 极致优化第一刀（VAL-stable + 字段两态 + sp 消，194→165ms）
SOLVED（单管线：helper 收敛，无新管线）/ 诚实记录未达预期部分

## 用户方向与容器结论
- Map/Set 调查结论（quickjs.c 56565-57900）：分离链接哈希，load 2.0，
  hash 不缓存、删除不缩。能动的（hash 缓存/mixer/预扩容）全是几十 %
  级别，2x 只在对抗性用例。**且 objshape 一个 Map 不用**（四相全是
  普通对象字面量 + 属性读写，走 shape/字段缓存路径）——容器线与
  objshape 线不相交，容器优化放下，对象路径往极致做。
- objshape 基线：whalejs 194 vs qjs 1203（6.2x 已赢）vs node 47（差 4.1x）。

## dump 定量（twoShapes 相，每 trip）
- 5 次 `JS_WhaleObjShape` helper 调用（每字段每迭代一次）；
- 5 组 sp/fv 检查 + tag 三目 + park + 字段写回。约 40 条语句/trip。

## 三刀（全 src/whale_jit.c 生成器侧）
1. **VAL-stable**：根因是对象局部一律统一成 VAL_T（put_loc 7727/set_loc
   7836 行实证；WHALE_OBJ_T 只活在栈临时，`cur[idx]` 永不到 OBJ），而
   stable 三处写死 `&& IS_OBJ(ta)` → 对局部恒假、stable 恒关。
   修法：VAL + 无循环内身份交换（新 `has_bcall` 字节码-call 门——has_call
   对对象循环恒真，门自相矛盾；+ 新 `shape_dirty`：循环内 OP_object 或
   写对象局部）同样 stable。soundness：威胁只有三条（加字段/删字段有
   put/def 检查；call 传出 reshape 有 bcall；slot 换对象有 dirty），
   put_field 只写值不碰身份。三处收敛进 `whale_field_stable()` helper。
2. **字段两态**（首 trip 全套 / 稳态直读写）：`fv == fver+1` 表已归一。
   首 trip：resolve + 值读（三目）+ 回写归一（tag=8）+ 置标记；稳态：
   读 1 条直取、写 0 检查直存。soundness：非数值读/写全 deopt（ObjValueNum
   无条件 deopt 实证；resolve 只收 8/0），shape 不变（stable），_wsc 每
   调用重置。5 calls → 首 trip 5 calls + 稳态 0。
3. **sp 消**：stable 访问点的 struct 列 guard 与 `sp=1` 重写全跳过
   （shape 证 presence 恒成立；note 保留，后读免查）。VAL-stable 189→
   169（5 calls 省了只值 ~5ms：helper 只是普通 C 调用，本来就不贵——
   之前 25ns/trip 估算错，认错）；两态 189→169（-11%）；sp 消持平
   （分支全中 + L1，玉米粒，诚实记录）。

## 测量（repeat=3）
- objshape 194→**165**（-15%）；全量 25/25 check 全一致；API 双路径绿。
- 到 node 47ms 还差 3.5x：剩余是 park 跨块（resolve 分支隔开读写，
  落地必需）+ fv 检查（每字段 1 读 1 分支）+ TCC spill。
  下一步是循环 peeling（首 trip 剥出，稳态循环零分支，park 可合）——
  单独一轮（动 loop buffer，风险中高）。

## 状态
绿，已提交 50e77df。下一步见 <107>。

# <107>: 分类 shape（8/2/3 一体）+ 小多态双槽 + merge 正确性修复
SOLVED（单管线：helper + 两态/poly 分支，无新管线）/ 真 bug ×1（merge 错读）

## 用户方向的取舍结论（8/2/3 一体，不冲突）
- 查地基：JSShape 只有 transition 链（1205-1226），无 dictionary/fast-slow
  之分；删除不建新 shape（原地 tombstone + 满 8 死一半才 compact）。
  tombstone（tag=3 + 非零 payload）在三套 ABI 下全 GC/free 不可见、
  `===undefined` 恒真——编码安全。
- 取舍：**transition 链不动**（解释器+全用户，风险大收益小）；
  **做 V8 式分类使用**：单态→两态/poly 双槽（JIT 消费 prof 的 top-4，
  不动引擎）；乱加字段→ transition 自然发散（不同加序不同 shape，
  本来就不共用——"自动不共用"零成本得到）；del 慢→我们的 tombstone
  已解（delete 原地标记，shape 不动，JIT 的列/tombstone 双读各走各）。
  和 3（transitioning store）不冲突：transition 边是引擎建的，
  JIT 只消费不断言——过渡存储是"发射查旧切新"，多态是"发射比对多旧"，
  同一套 fs/fp 槽。
- 和 CStruct 不冲突：CStruct 是存储变换（对象消失），分类 shape 是
  查找加速（对象还在）。函数内部分：能完全拆箱的走 CStruct（对象不
  逃逸），不能的（逃逸/多态）走分类 shape——判据是现成的逃逸门，
  不是新切换。

## 落地的：小多态双槽 poly-2（load/resolve）
- prof 已有 top-4（无计数，首见顺序≈热度）。恰 2 shape 的站发双槽：
  `S==fsA ? fpA : S==fsB ? fpB : 全套（填B + 首填回填A）`。
  第三 shape 全套 resolve（不 fence 宇宙，不可解才 deopt——和 deopt
  想法同构：轻决策，不是重编译）。
- 5 处调用点：fc2 分配（24 对预算内）+ `two_off` 全局传站（签名已长，
  不再加参）+ 两态加 `fc2<0` 前提（**关键正确性**：两态的 fv 标记分不
  清同 slot 两身份，站位多态必须走 poly——不加则静默错读）。

## 真 bug ×1：merge-obj 臂错读（静默错答案，已修）
- `o=o1`（fill 列，STRUCT）与 `o=o2`（copy 槽，VAL）在 join 相遇，
  merge 选 VAL 丢列 → 奇 trip 全读旧对象（97.75M vs 55M 实证）。
  根因：臂假设"STRUCT 列恒 stale"，两边都新时不成立。
- 修法：loop head（entry 旧/backedge 新）才选新；体内 join 的 STRUCT/VAL
  同 slot 分歧一律 differ 拒编（安全方向，setField 形在 head 不受影响）。
- 附带修：三元 `?:` 存对象的 L18 缺分支是既有 bug（if/else 版正常），
  与 poly 无关，不碰。

## 测量
- poly-2 验证：字面量交替形（fill 关）dump 确认双槽 + A 回填 + check 对；
  开 fill 形因 merge 新规则拒编转解释器（对但慢——fill×poly 同 local 交替
  仍是 open 问题，记下一步）。
- 全量 25/25 check 全一致；objshape 167（repeat=3），bitops 128，无退化。
- 本轮是机制落地（混合场景的对象版），benchmark 无 poly-2 形状故无大数；
  真实业务（可选字段交替）吃到即赚。

## 状态
绿，已提交 5944eb9。下一步见 <108>。

# <108>: objshape 分相 + shape 归一评估 + pin 豁免试错回退
SOLVED（分相计时）/ 试错回退 ×1 / 评估结论 ×2（不做，诚实记录）

## 分相计时（whalejs vs node，repeat=1）
- twoShapes 16 vs 49（已超 3x）、growsShape 23 vs 29（已超）、
  writeOrder 23 vs 5（4.6x）、escaped 121 vs 22（5.5x，占总量 66%）。
- 总量 183 vs 105。node 47ms 线（全量口径）的主攻点是 escaped。

## 试错回退：field-stable 不杀 pin（writeOrder 21→18，但 objshape +10%）
- 根因定位：writeOrder 7 语句 0 调用全 local，21ms 全是 spill；
  `put_field` 不在 purity 表 → has_call 恒 1 → L-pin/`_wv` 全灭。
- 试了 heat 扫 field 豁免（shape_stable 即免）：writeOrder 21→18，
  但 objshape 165→182（repeat=3 确认，+10%）：pin 占 pool 挤压转换链
  （pin 是访存型优化，对象循环一律不开——门控策略 <101> 的实例）。
- 保大放小：已回退（注释留结论），objshape 回 166，全量绿。

## 评估结论 ×2（不做）
1. **escaped 小修全玉米粒**：死值/死分支（`is_undefined` 数值恒假 +
   `if((0)!=0)` 空跳，零执行成本）、搬运、sp 重写，共 ~3ms。不做。
2. **Define transition 缓存不做**：只能省查找（~5ms），分配铁板
   （prop 数组 + Float64×2，每 trip 3 malloc ≈ 100ms）要复用/nursery
   才消（风险高/GC 大活）。escaped 真解是槽位对象复用或 nursery，
   本轮不动（121ms 认）。
3. **引擎级 shape 归一不用做**：order-insensitive + slack（小共用大+2），
   JIT union 层已隐含（<63> 分相 + 集合语义布局，与序无关；缺席列
   presence=0/tombstone）。引擎动 transition 是解释器热路径，起飞风险
   实锤。twoShapes 16ms 超 node 3x 就是 union 的活证据。

## 测量
- 全量 25/25 check 全一致；objshape 166、bitops 128、objfields 27，
  余无变化（repeat=3 确认关键三项）。
- 到 50ms 线还差：escaped（复用/nursery，大活）+ writeOrder（spill，
  线性 IR）+ peeling（稳态零分支）。frontend 能动的已基本收净。

## 状态
绿，已提交 c562cb5。下一步见 <109>。

# <109>: escaped 槽位复用（objshape 166→74ms，-55%，反超 node）
SOLVED（单管线：loop 表 + ru 标记，无新管线）/ 弯路 ×3（全记录）

## 打法（peeling 让路：只值 ~20ms，复用值 ~80ms）
- 分相定死 escaped 占 66%（121ms，4 calls + 3 malloc/trip）。
- peeling ROI 重估后放弃（fv 检查 5×2 cycles + park 跨块，满打满算 -15%，
  还要动 loop buffer）。复用才是 50ms 的必经之路。

## 落地：循环内建对象槽位复用
- 识别 `whale_loop_reuse_of`（仿 memset 骨架）：体内单 OP_object +
  数值 define/put/get_field + 单 put_array_el + 无 call + 无内 head +
  单 exit。write-only/escape 证明 `reuse_writeonly`：数组 local 由
  array_from-put 配对定（put 前回扫分不清数组/下标/值）；
  无 get_array_el/length 读数组、无 call、无全局/闭包存、无 with；
  return 只收常量/数值 local（loc_obj 查）/数值 binop/未作对象用的 arg/
  return_undef。
- 发射三处：OP_object `if (!ru) 建` + define 首 trip Define/稳态
  StoreNum（现成 helper，缓存指针零 transition）+ put_array_el 的
  STRUCT 物化点 `if (!ru) 物化`（**真落点**：t 走 frame 列，ObjNew 在
  物化点不在 OP_object——查了三轮才定）。稳态每 trip 1 call（ArrSetVal）。
- `_wsc.ru[4]` + head→idx 映射（4 封顶，超了走老路）。

## 弯路 ×3（教训）
1. 复用门一开始装在 OP_object/define（通用对象路），但 t 走 frame 列，
   真正分配在 put 的 STRUCT 物化点。`[reuse-case]` 打印证明 case 跑了
   但 fallback 没跑——定位工具，不是瞎猜。
2. opcode 号段重叠：194 既是 push_i8 又是 scope_put_var_init（短/临时
   共用设计），escape 名单误杀小整数 push。修：删名单 + with 块另拒。
3. return 证明三洞：`return acc+seed` 前是 add（数值 binop 结果恒数值，
   放行）；get_arg 要查 args_obj/arr；get_loc 要查 loc_obj。逐个补。

## 测量（repeat=3）
- escaped 相 121→**19ms**（单相，反超 node 22ms）；objshape 166→**74ms**
  （-55%），node 47 只差 1.6x。全量 25/25 check 全一致；API 双路径绿。
- 到 50ms 还差：writeOrder spill（线性 IR）+ escaped 剩余 ArrSetVal
  （1 call/trip，数组语义必需）+ 通用 margin。

## 状态
绿，已提交 50f8a96。下一步见 <110>。

# <110>: deopt 双版本第一步（census + 通用版 + 选择器，re-JIT 执行下轮）
SOLVED（单管线：计数 + 开关，无新发射逻辑）/ 大坑 ×1（对齐）

## 用户策略的拍板（两级 key + hook 只管新鲜装箱）
- L1（函数级）：argc + 参数类型/shape 类，L1 不同 = 不同函数，独立版本表。
- L2（同 L1 内 shape 演进）：3 特化槽 + 1 通用槽（key 去 shape，不存指针）。
  前缀兼容（只追加）让每槽覆盖"一 shape 及其全部后代"。
- hook 只管新鲜装箱（返回/字面量新建，硬编码序列不变），参数原地改
  永远写回（aliasing 洞，发射 review 红线）。二进制 hook 先不做，用参数提示。
- 释放：槽上限 4（结构性封顶）+ 单版本一窗口零调用即释；通用版永不淘汰；
  冷对象醒来走通用版（最多慢一次，不会错）。

## 落地的（本轮：census + 通用版 + 选择器读数，不自动切换）
1. **census**：stats 加 deopt_shape/tag/guard 三计数 + `JS_WhaleDeoptWhy`
  （纯计数 + DBG，只跑 deopt 分支）。埋入：shape 6 处（fp-NULL）、tag
   3 处（tag 检查）+ ObjValueNum 内记 1 处。guard 类派生（总数-shape-tag）。
2. **通用版**：首编形态命名化——5 处 summary 降级开关（shape_stable、
   prof_one_shape、int_ok、mono×2），WHALE_GENERIC 驱动。objshape：
   specialized 77 / generic 465 / interp 1200——通用版甩开解释器 2.5x
   （"次高性能"成立），代价是 specialized 的 6x。
3. 选择器读数：census 验证（交替 shape 站每次记 shape-miss ✓）。

## 大坑 ×1：deopt 分支加调用，纯数值循环 -30~50%
- bisect 定案：guard 分支（int-region 入口 guard + arg unbox）的 DeoptWhy
  调用改变循环对齐。退化全中 int-region 循环（arith 64→107 等），对象类
  不动。dump 对比排除行为变化。
- 修法：guard 类零发射（总数派生），census 只留 shape/tag（对象循环分支
  本来就多，不敏感）。数字全回来。全量 25/25 绿。
- 教训：TCC 生成码对"循环前分支体大小"敏感到 50% 量级——以后任何循环前
  新增发射（guard/计数/ slow 路）都要过一遍纯数值计时。记为门控新条目。

## 测量（repeat=3）
- census：交替站每次 shape-miss ✓；全量 25/25 check 全一致。
- 三配置（objshape）：77 / 465 / 1200。bitops generic 持平（无对象，
  int-region 关了也一样——和 <104> 结论一致）。
- 通用版 entries 44 vs 48（4 函数 fc 预算超限 bail 转解释器，安全方向，
  记 tech-debt：fc 预算可放宽）。

## 状态
绿，已提交 db0cb70。下一步见 <111>。

# <111>: deopt 双版本第二步（版本表 + 分发 + 入槽 + 冷淘汰，全闭环）
SOLVED（单管线：表 + 查 + 编，无新发射逻辑，调用点 0 语义改）

## 用户拍板的落地（L1=不同函数独立表，L2=3+1，上限结构性封顶）
- 版本表（quickjs.c，函数字节码上）：3 特化槽（entry/tcc/key/use）+ 1 通用
  槽（永不淘汰）+ 时钟/miss 计数/miss key。key = 对象参数 JSShape 指针
  异或（unbox 路已取，零新增 load；argc 混入）。无对象参数 key=0 → 走
  现有单版本路（纯数值零 regression）。
- 分发（两调用点各 +10 行）：表空/无对象 → 原路；命中 → 切 entry 直调
  （self-check 参数保持主版本，entry2 逻辑正交不动）；miss → 记 miss+
  通用版（有则调）/主版本，无通用则主版本 + census。
- 首编即编通用版（objs>0 才编，纯数值免）：特化 + 通用一次做完（阈值触发
  时）。`info.generic` 字段（WHALE_GENERIC env 照旧 OR）。
- 入槽 + 淘汰（on_call，ver_miss>=8，同 deopt retire 阈）：编新特化（prof
  常开，新 shape 已在表）→ 空槽优先，满则 LRU 淘汰最冷特化；通用版永留；
  miss key 在 pick 时记，rejit 用。释放 = 指针拿掉（tcc 句柄 no-op，
  内存上限结构性封顶）。
- hook 只管新鲜装箱的验证：装箱序列零改动（硬编码照旧），本轮没动装箱；
  参数原地改写回路径零改动（review 红线守住：无 put 到参数对象的重装箱）。

## 验证（verswitch：前 100 次 a，后 200 次 b）
- 首编特化（a）+ 通用版一次做完；切 b 后 miss 进通用版；miss 满 8 编新
  特化入槽 0（`parked slot 0` 日志）；check=2400 全对（100×2+200×11）。
- 全量 25/25 check 全一致；objshape 75（repeat=3），bitops 128。
- 首编成本 ×2 只发生在对象函数（objs>0 才编通用版）；纯数值单编不变。

## 状态
绿，已提交 dd6f8d4。下一步见 <112>。

# <112>: 前缀兼容（一槽罩一族，后代 shape 零重编）
SOLVED（单指针 key + 前缀判定，无新表）

## 地基（查过才动手）
- JSShape 无 parent 指针、hash 不可逆——祖先不可追溯，只能逐项比。
  `find_hashed_shape_prop` 的单步扩展检查就是现成模板，照它写多步。
- key 重定：单对象参数 key=shape 指针本身（malloc 对齐低位恒 0）；
  多对象 key=xor|1（低位置 1 区分）。0 表"无对象"，永不命中。

## 落地（quickjs.c，两处）
- `whale_shape_is_descendant(D,S)`：同 proto + D 字段数>= + 前缀
  atom/flags 全等。compact 过的形状可能误判失败→走通用版（安全方向）。
- pick 单对象分支改前缀匹配；多对象精确匹配照旧（组合不值）。
- 主版本进槽 0（key=首编 prof 首 shape）：否则首编版本游离表外，
  后代到来无槽可命——实测两次入槽，修后一次命中零重编。
- soundness：前缀保证老字段同偏移（读对）；版本内只读写已知字段
  （新字段碰不到）；装箱按版本 shape 建（新建与传入无关）；参数
  原地改写老偏移（同偏移）。

## 验证
- verprefix（A={x,y} → B={x,y,z} 后代）：两次编译（特化+通用）后
  零重编，check=1500 全对。修前编三次 + 两次入槽。
- 全量 25/25 check 全一致；objshape 76、sieve 64（repeat=3）。

## 状态
绿，已提交 47c3e6c。下一步见 <113>。

# <113>: key 补全（argc + 位置）+ 冷淘汰长跑验证
SOLVED（key 共享 helper + on_call 传参）/ 长跑行为记录

## key 洞（实证排出，非猜想）
- 旧 key（纯 shape 异或）：`f(o)` 与 `f(o,x)` 同 shape 会撞槽；
  `f(o,1)` 与 `f(1,o)` 同样撞。撞槽不翻车（版本按首编 shape 编，
  错 key 进错版本读错偏移），但必须堵。
- 修法：64 位 key 高 16 位编 argc（8）+ 对象位置（8），低 48 放 shape
  指针（malloc 只用低 48）。匹配改 argc/位置精确 + shape 前缀。
  `key_of` 抽 helper，pick 与 on_call 首编登记同语言。
- 主版本进槽曾用 prof 首 shape（裸指针，无 argc/位置）→ 永不命中。
  改 on_call 传实时 key（阈值到达那次调用；backedge 传 0 占位）。

## 长跑（5 shape 轮转，LRU 抖动）
- check 全对；17 次入槽（3 槽罩不住 5 活跃 shape，轮转 miss→通用版→
  重编→淘汰，循环）。通用版兜底生效（值全对），抖动期 40% 通用版速度。
- 结论：设计内行为（V8 mega 态同理退化）。治抖动的"轮转检测钉通用版"
  记 tech-debt，本轮不做（正确性无损）。
- 跨 arity/跨位用例（f/g 同 shape 不同签名）check 全对，无碰撞。

## 测量
- verprefix（后代 shape）：零重编（三次编译→两次），check 全对。
- 全量 25/25 check 全一致；objshape 73、nested 21（repeat=3）。

## 状态
绿，已提交 f7ae72d。下一步见 <114>。

# <114>: loop 级 stable（循环外加字段不再杀整函数）
SOLVED（单循环函数，predef 集，6 处 call site 收敛）/ 诚实记录零全量收益

## 动因（分相现状）
- objshape 全量已超 node（总量 78 vs 86）：twoShapes 超 2x、escaped 超、
  growsShape 持平、writeOrder 2.9x（纯 spill，线性 IR 的活）。
- 50ms 差 28ms：writeOrder 13ms + 通用 margin。loop-stable 瞄准 growsShape
  （`o.r=0` 循环外加字段杀整函数 stable）。

## 落地
- per-head 表（单循环函数才算，多循环免 bookkeeping）：体内 put 全命中
  define 集 + 循环前 put 集（pre-loop store 早长一次，循环内 shape 固定）
  + 体内无 call → 该循环 stable。
- 6 处 `field_stable` call site 全换 `|| loop_stable_here()`（depth/head 查表）。
- 表挂接：calloc/fill/publish/free+NULL 全套（free 漏了会野指针，review 点）。

## 诚实记录
- growsShape 22→20ms（只拿 2ms）：它本来就被 prof 单态救了（两态早就在），
  loop 表只是把"prof 路"换成"表路"，同形态零收益。
- 全量零收益（repeat=3 确认关键项），但机制正确（首编无 prof 时生效，
  冷启动形状）。保留。
- 括号事故一次（replace_all 多个括号，构建即抓，无后果）。

## 测量
- 全量 25/25 check 全一致；objshape 75、sieve 64（repeat=3）。

## 状态
绿，已提交 049f1b2。下一步见 <115>。

# <115>: writeOrder 评估（AST+缓存约束下无利可图，认账）
零代码改动轮（只读 + 计时，全量确认）

## 评估（用户约束：不要 IR，最多 AST+缓存）
- writeOrder 7 语句 0 调用全 local：数据流 4 级依赖链（%→+1→+→acc+），
  `%` 只求一次是铁律（idiv 26 cycles，求两次净负）→ 最少 6 条语句。
  最优变换（L5 命名 + 全嵌套复读）省 1 store + 1 load ≈ 2ms。玉米粒，不做。
- 换变量名（_wd 镜像）零收益：TCC 对一切 C 局部一视同仁 spill。
  出口活性分析（L3/L4/L5 出口死）省的是装箱（本就没有，deferred），
  不是 spill。不做。
- 结论：spill 主体只能寄存器分配（后端，动 TCC 分配器，800 行级）或接受。
  writeOrder 21ms（node 7 的 3x，TCC 天花板）认账。

## 到 50ms 的诚实算账（总量 78）
- writeOrder 13ms 差值：要线性 IR，无 AST 解。
- escaped 22ms：稳态 1 ArrSetVal（数组语义必需）+ 列 spill，无 AST 解。
- 通用 margin：分支预测/icache，调不动。
- 结论：50ms 在"不要 IR"约束下到不了。当前 78（已超 node 86）是
  AST+缓存的天花板。再往下只有两条路：后端寄存器分配，或 deopt 那套
  （轻决策把工作推给版本选择——已落地，见 <110>~<113>）。

## 测量
- 全量 25/25 check 全一致（repeat=1）；objshape 79、locals 81、ident 54
  （repeat=3 确认关键项，repeat=1 偏差全是噪声）。

## 状态
绿（零改动，无提交）。下一步：后端寄存器分配（用户放行才做），或多签名
L1 表，或 deopt 那套的冷淘汰长跑（已验证）/前缀兼容（已落地）。

# <116>: 逗号融合落地（sink-mark 方案证伪 → output-tail 重写）
SOLVED + 真 bug×2（双提交 + 陈旧状态）

## 起点
<115> 认账之后用户说"继续"：逗号融合（连续纯存储 `L5=..; L4=..; L3=..`
合成一条 `L5=.., L4=.., L3=..;`，TCC 每语句边界 spill 一次，语句少即
spill 少）是 AST+缓存约束下唯一还没动的前端语句级优化。

## 第一版方案（sink-mark）与证伪
状态 = open + 序列起点 sink mark + 上段尾 sink mark；相邻（`mark ==
comma_end`）则续接 `,`，否则 close（补 `;` + 提交）。三天症状
（`';' expected` / `_wsv` 名丢失 / `duplicate label` 跨 phase 跳动）全是
同一个根因的不同侧面：
- `[commit L3891] mark=1680 n=1776`（fold_materialize 的 eager commit）
  紧跟 `[commit L3164] mark=1681 n=1777`（close 的 recommit）——同一
  sink 区被提交两次，第二次缺首 token。
- 完整 mirror 定案：`_wsv[0]=(EXPR) [0]=(EXPR) ;` —— 重复存储，第二份无名。
- 结论：eager-commit（found 时提交）+ close-recommit（close 时再提交）
  在设计上重叠，sink 相邻性判据救不了。第一版整套作废。

## 第二版（output-tail 重写，落地）
- 语句永远完整提交（含 `;`）；open 序列 = 当前输出 buffer 里一条已提交
  语句（记 base + len）。续接 = `whale_comma_continue()` 校验尾（同
  buffer + 长度吻合 + 尾是 `;`）→ 弹 `;`、推 `,` → 新存储提交（一次
  commit，无重叠可能）。helper 语句插中间则尾校验失败，自动断链。
- close 只清状态（不再提交）；`fold_begin` 补状态复位（旧版陈旧 mark
  跨编译泄漏，第二编译一起点就是脏的）；`fold_set` 超长门删 save/restore
 （提交即完整，无外层序列可扰）；hoist 改搬运输出 token（旧版从 sink
  recommit 会重发整条 open 链）+ 仅 founded 语句可 hoist（续接进来的旧链
  前段可能 variant，搬出去等于重复首 trip 的值）。
- 调试脚手架（commit 行号打点、vname 打点、mirror 断尾补全）全拆，只留
  whale_jit.c 改动（+155/-9）；iface.c 零改动。

## 验证
- bitops 循环体 5 存储融成一条（dump 第 108 行
  `I2=.., _wv0=.., L0=.., _wv1=.., L0=..;`），writeOrder 双版本损坏消失。
- 全量 25/25 check 全一致；repeat=3：bitops 128、locals 87、objshape 75、
  nested 19、loop_sum 44、arith 62。
- 诚实记录：融合点火但性能零变化——TCC 在大逗号表达式内部同样 spill
  临时值，省掉的只是语句边界，表达式内部的求值栈 spill 还在。这与 <105>
  的"语句合并 ROI 放弃"结论一致：TCC 的 spill 是按求值深度不是按语句数，
  逗号融合的上限就是零。机制保留（正确且零成本），收益指望转后端。







