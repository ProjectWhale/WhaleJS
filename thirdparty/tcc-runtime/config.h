/* TinyCC 构建配置（内容等价于 win32/build-tcc.bat 生成的那几行）。
 *
 * 为什么落库而不是构建时生成：TCC 的 configure 是 shell 脚本，本机（Windows/PowerShell）
 * 没有 sh，跑不了；而 win32/build-tcc.bat 又只用 MSVC/GCC 手写这几行。版本跟着上游走，
 * 目标固定为 x86_64 + PE。
 */
#define TCC_VERSION "0.9.27"

#ifdef TCC_TARGET_X86_64
#define TCC_LIBTCC1 "libtcc1-64.a"
#else
#define TCC_LIBTCC1 "libtcc1-32.a"
#endif
