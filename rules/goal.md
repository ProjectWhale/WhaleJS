# 项目目标
使用QuickJS-ng和TinyCC（已停更，可以放心做侵入式更改）实现改造版的QuickJS，以此支持JIT和AOT
分为两个版本：
- Full：支持更多JS引擎能力，向浏览器API看齐
- Lite：只是为JS

这个JS引擎将会和github.com/THZrry/WhaleUI（不用下载到本地分析）联合组成一个轻量浏览器引擎，二者功能交叉需要预留对接空间（如WhaleUI提供多媒体播放、DOM底层、事件，而本项目负责调用）

# 搭建环境
下载QuickJS-ng（https://github.com/quickjs-ng/quickjs）和TinyCC（https://bellard.org/tcc/）并解压到临时目录，使用xmake管理项目。
以下是Full需要的额外功能，可以选择thirdparty链接或xrepo下载，优先选择预编译版本直接链接：
网络功能：libhv（https://github.com/ithewei/libhv）
安全功能：mbedtls（https://github.com/Mbed-TLS/mbedtls）
跨进程交互（如相关API和ffmpeg集成）：reproc（https://github.com/DaanDeMeyer/reproc）
wasm支持：wasm3（https://github.com/wasm3/wasm3）

# 实现本项目
阅读TinyCC源代码，先做好规划

## JIT和AOT的实现
由于QuickJS是不断更新的项目，你应该尽量非侵入的修改，让它保持兼容，或者在适当的位置进行修改，以将代码跳出到自定义的代码中。
你需要使用TinyCC生成字节码，将JS代码或虚拟字节码直接转化为机器代码，而不是将虚拟机代码包装成JIT。你需要至少支持x86_64和arm，也要支持关闭JIT。
对于短时间多次调用的JS函数，你可以分析调用链，对热点函数进行编译，而纯JS（不调用过多外部API，仅仅完成大规模计算的代码）可以标记，在空闲时直接AOT为机器代码。

## JS API本身
对于Promise、Proxy、Stream等功能，QuickJS不一定已经全面支持，你需要先用C/C++补全相关定义和功能。

## 功能接入
对于fetch等功能，必须使用第三方库，为Full接入第三方库并且完整实现这些功能。
对于多媒体，你需要使用ffmpeg cli完成功能，使用reproc解决。当前还无法渲染，但你要把功能预留出来。
对于DOM和事件API，你只需要预留JS侧完整的定义即可，不用实现，但需要能很好的接入。

## Debug功能
实现debug，实现Runtime各种API，方便接入Chromium DevTools。

## 测试
编译一份原版QuickJS-ng，编译本项目，使用fibonacci进行JIT功能验证。单元测试也要求跑通，支持v8引擎的多数功能。
