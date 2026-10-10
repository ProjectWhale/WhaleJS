# WhaleJS Changelog

WhaleJS 自己的版本（MAJOR.MINOR.PATCH），与 QuickJS-ng 的版本解耦：
上游升级时只合并它的代码，绝不跟着它改这里的号。
`whalejs --version` 同时打出两个版本。

规则：MAJOR = 对外接口 / JIT ABI 不兼容；MINOR = 新优化 / 新 opcode
覆盖；PATCH = 纯修 bug。

## 0.1.0

第一个可用版本：TinyCC JIT 跑通，perftest 25/25 全绿（0 mismatch）。

- 数值特化（double 域 + guard + deopt 整帧重跑）与表达式融合
- 比较-分支融合、位运算短式（ToInt32/`%`/`!x`）、热局部写点物化
- 小 trip 循环展开（minmax 追平 node）、Uint8Array fill/count 整循环消除
- 对象：CStruct 列存储、free list、原地 fusion（immutable 进 100ms 线）
- 后端：TCC 单例多链接、gcc 按函数二级编译、ISA 分级框架（CPUID+覆盖）
