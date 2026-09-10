# JIT / AOT 设计（草案 v0）

## 已确认的引擎内部事实（源码依据）
- 字节码定义：`src/quickjs-opcode.h`。表项宏 `DEF(id, size, n_pop, n_push, fmt)`，操作数格式由 `FMT(...)` 列表决定
  （`u8/i8/loc8/const8/label16/atom_u16/...`，见文件开头）。
- 执行核心：`src/quickjs.c` 里的 `JS_CallInternal()`（大 switch 解释器），`DIRECT_DISPATCH` 宏控制计算跳转。
- 栈帧：`JSStackFrame`
  `{ prev_frame, cur_func, arg_buf, var_buf, var_refs, cur_pc, var_ref_count, arg_count, is_strict_mode, is_constructor, cur_sp, cur_gc_obj }`。
- 函数对象类：`JS_CLASS_BYTECODE_FUNCTION`；运行时：`JSRuntime`，上下文：`JSContext`。
- 值类型 `JSValue`（NaN-boxing）；GC = 引用计数 + 环收集；分配走 arena（`JSMallocBlockHeader`），
  GC 标记位就存在分配头里 → **JIT 代码里的 JSValue 必须落在引擎能扫描到的地方**。
- 中断/栈限制：`interrupt_counter`、`rt->stack_limit`，解释器在循环里检查。

## TinyCC 集成接口（thirdparty/tinycc/libtcc.h）
`tcc_new / tcc_set_output_type(TCC_OUTPUT_MEMORY) / tcc_add_symbol / tcc_compile_string /
tcc_relocate(s, TCC_RELOCATE_AUTO) / tcc_get_symbol` —— 全部在内存里完成编译+重定位，正好给 JIT 用。
AOT 则用 `tcc_output_file` 落盘。

## 路线
1. **编译单位**：`JSFunctionBytecode`（按函数编译），首次慢速执行后进入候选。
2. **翻译方式**：字节码 → C 源码（每个 opcode 展开成等价 C 序列；值栈用 `JSValue` 数组；计算类走内联，
   对象/慢路径调 `quickjs.c` 里的内部辅助函数，如 `js_add_slow` 一类的 `slow` 变体）→ libtcc 编成机器码。
   **不自己写 x86_64/arm 汇编后端**，靠 TCC 的 `x86_64-gen.c` / `arm64-gen.c`。
3. **热点策略**：`JSFunctionBytecode` 加 `call_count`；超阈值触发 JIT。
   纯 JS 函数（无外部 API 调用、无 eval/with、闭包不逃逸）标记为可 AOT，空闲时编译并缓存到磁盘。
4. **挂接点（非侵入）**：`JS_CallInternal()` 开头判断 `func->jit_entry != NULL` 则跳转 JIT 代码；
   不支持/编译失败/异常一律回退解释器。上游更新时只需重放这个 hook。
5. **可关闭**：xmake option `jit`（默认 on）；关闭时源码不启用任何 hook 行为，等同原版。
6. **平台**：先 x86_64，再 arm64（TCC 有后端）。无 JIT 平台也能构建、跑解释器。

## JIT 函数 ABI（草案，待定）
```c
typedef JSValue (*JSJitFunc)(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, JSValue *locals);
```

## 先不做 JIT 的形态
generator / async / eval / with / 动态作用域；含逃逸闭包（`var_refs`）的函数第一版只做「无捕获」或「只读捕获」。
先保证 fibonacci 这类纯计算热点函数跑通。

## 风险 / 待办
- **异常传播**：解释器用 `goto exception`；JIT 侧需返回 `JS_EXCEPTION` 哨兵，由 `JS_CallInternal` 统一处理。
- **GC 可见性**：JIT 帧里的 `JSValue` 必须在引擎可扫描的栈/`var_buf` 上，否则漏标 → 提前回收。
- **中断与栈溢出检查**：JIT 的循环回边要插 `interrupt_counter` / `stack_limit` 检查（可按循环加计数器）。
- **待细读源码**：`JS_CallInternal()` 全文、`JSFunctionBytecode` 布局、各 opcode 语义与 `slow` 辅助函数、
  `quickjs.h` 公开 API、`tcc_relocate` 的符号重定位限制（尤其 Windows PE 下的 COFF 重定位）。
