# Nexus Chinese

这是一个实验性的 Guild Wars 2 Nexus 插件项目，目标是把 [cy-sp-howard/lang5](https://github.com/cy-sp-howard/lang5) 中的中文 UI 切换思路移植到 Nexus 插件环境。

当前项目还不是稳定可用的发布版，也不是原 Lang5 的完整替代品。

## 当前状态

- 已实现 Nexus 原生 DLL 原型。
- 已能扫描并定位 GW2 中和语言切换相关的地址。
- 已实现诊断日志，用于确认当前 GW2 版本的地址是否仍然有效。
- 正在实验类似原 Lang5 的 `ViewAdvanceText` 延迟调用方案。
- 尚未实现简体转繁体、文本替换、字典加载等原 Lang5 功能。

## 测试环境

当前测试环境记录于 2026-05-15：

- Apple M2 Pro
- macOS 26.4.1 (25E253)
- CrossOver Version 26.1 (26.1.0.39808)

## 风险提醒

这个插件会读取、扫描并在实验模式下修改 GW2 进程内存。

使用它可能带来以下风险：

- 游戏崩溃。
- GW2 更新后地址失效。
- 和 ArcDPS、ReShade、Blish HUD Lang5 或其他内存类插件冲突。
- 可能不符合 ArenaNet 对第三方程序的政策要求。

本项目没有得到 ArenaNet、Raidcore、Nexus、Blish HUD 或 Lang5 原作者的认可。请只在你理解风险的前提下用于本地测试。

## 目录

```text
lang5-nexus-cn/              Nexus 插件原型
blishhud-to-nexus-migration/ Blish HUD 到 Nexus 的迁移笔记
```

## 构建

进入插件目录：

```sh
cd lang5-nexus-cn
```

诊断版构建：

```sh
cmake -S . -B build-mingw-gcc -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-mingw-gcc --config Release
```

当前实验版构建：

```sh
cmake -S . -B build-mingw-deferred \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
  -DLANG5_ENABLE_UNSAFE_LANGUAGE_CALL=ON \
  -DLANG5_ENABLE_DEFERRED_CALLER_HOOK=ON \
  -DLANG5_ENABLE_DIRECT_SETTER_CALL=OFF \
  -DLANG5_ENABLE_MEMORY_WRITE=OFF
cmake --build build-mingw-deferred --config Release
```

## 测试建议

先测试诊断版：

```text
lang5-nexus-cn/dist/Lang5NexusCn.dll
```

确认不会崩溃、日志正常后，再测试实验版：

```text
lang5-nexus-cn/build-mingw-deferred/bin/Lang5NexusCn.dll
```

如果游戏崩溃，请先从 Nexus 插件目录移除 DLL，再重新启动 GW2。

## 来源

本项目源于 [cy-sp-howard/lang5](https://github.com/cy-sp-howard/lang5)。原项目是 Blish HUD 模块，本项目是 Nexus 原生插件实验。

`lang5-nexus-cn/include/Nexus.h` 来自 Nexus API，授权信息见 `lang5-nexus-cn/NEXUS_API_LICENSE`。
