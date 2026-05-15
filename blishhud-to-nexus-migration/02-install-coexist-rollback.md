# 安装、共存和回退

## Windows 安装 Nexus

官方推荐两种方式：

1. 使用 Nexus installer，它会自动检测已有修改。
2. 手动下载 `d3d11.dll`，放到 GW2 安装目录，也就是 `Gw2-64.exe` 同级目录。

手动安装后，右键 `d3d11.dll` 打开属性，如果看到 Windows 的 Unblock / 解除锁定选项，需要勾选。

启动游戏后，Nexus 会弹出第三方软件风险确认窗口。接受后即可在游戏里打开 Nexus 界面。

## 已经有 ArcDPS 或其他 d3d11.dll 怎么办

GW2 目录里同名的 `d3d11.dll` 只能有一个，所以要处理链式加载。

常见做法：

- 如果当前 `d3d11.dll` 是 ArcDPS，可以删除后通过 Nexus 重新安装 ArcDPS。
- 或者把 ArcDPS 及其插件移动到 `<GW2>/addons`，交给 Nexus 管理。
- 如果当前是其他 DX11 proxy，把原来的 `d3d11.dll` 改名为 `d3d11_chainload.dll`，再把 Nexus 的 `d3d11.dll` 放进游戏目录。

## 和 Blish HUD 共存

Blish HUD 是独立程序，不依赖 GW2 目录里的 `d3d11.dll`，所以可以和 Nexus 同时运行。

推荐共存顺序：

1. 先保持 Blish HUD 原样。
2. 安装 Nexus。
3. 在 Nexus 内逐个安装替代 addon。
4. 每迁移一个功能，就在 Blish HUD 中关闭对应模块。
5. 连续游玩几天确认稳定后，再决定是否停用 Blish。

这样做的好处是出问题时容易定位：只要重新打开 Blish 模块，或临时禁用 Nexus addon，就能快速比较。

## 回退到没有 Nexus

回退很简单：

1. 关闭游戏。
2. 从 GW2 安装目录删除 Nexus 的 `d3d11.dll`。
3. 如果之前有 `d3d11_chainload.dll`，按需要改回 `d3d11.dll`。
4. 重新启动游戏确认无 Nexus 弹窗。

如果你通过 Nexus 管理了 ArcDPS，回退后需要重新按 ArcDPS 原来的方式安装。

## Linux / Steam Deck 经验

社区经验里，迁移动力最多来自 Linux 环境下 Blish HUD 覆盖层黑屏、输入异常、透明层处理麻烦等问题。常见替代方案是：

- Nexus 作为 addon manager。
- TaimiHUD 提供路径、标记、计时等 TacO/Blish 类功能。
- 标记包目录可能需要手动创建，例如 `GW2/addons/TaimiHud/pathing` 一类目录，具体以 TaimiHUD 当前设置界面为准。

注意：Linux 环境差异很大，Steam、Lutris、Bottles、Crossover、Proton-GE 的路径和行为都可能不同。迁移时先只启用 Nexus + 一个 addon 测试，不要一次性装满。
