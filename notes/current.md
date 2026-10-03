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
