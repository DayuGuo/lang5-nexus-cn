# 排错、风险和经验

## 启动后 Nexus 没出现

检查：

- `d3d11.dll` 是否放在 `Gw2-64.exe` 同级目录。
- 文件是否被 Windows 锁定，属性里是否需要 Unblock / 解除锁定。
- 游戏是否确实使用 DX11。
- 是否被其他 `d3d11.dll` 或 DX11 proxy 覆盖。

## 已有 d3d11.dll 冲突

同级目录只能有一个主 `d3d11.dll`。如果之前装过 ArcDPS、ReShade、GW2 Addon Loader 或其他 proxy，需要决定谁作为主加载器。

迁移到 Nexus 为主时，通常让 Nexus 占用 `d3d11.dll`，其他 proxy 走 Nexus 管理或 `d3d11_chainload.dll`。

## Blish 和 Nexus 界面重叠

共存阶段可能出现两个 overlay 都显示计时、路线、鼠标增强、提示窗口。处理方式：

- 每次只迁移一个功能。
- Nexus 替代功能稳定后，在 Blish HUD 中关闭对应模块。
- 保留 Blish 只负责 Nexus 没替代的模块。

## Pathing 迁移不完整

Blish Pathing 是 Blish 的强项。官方说明它支持大量标记包特性、社区标记包、一键下载和自动更新。社区迁移经验里，TaimiHUD 可以覆盖不少路径需求，尤其在 Linux 上有价值，但也有人反馈并非所有 Blish 功能都有同等体验，例如心形任务提示、路径线、地图边界、鼠标模块体验等。

建议：

- 先选一个常用标记包测试。
- 测试你最常跑的路线，而不是只看能不能加载。
- 对成就提示、路径线、地图图标、Waypoint 复制等细节逐项确认。
- 如果少一个关键功能会影响体验，就继续保留 Blish Pathing。

## Linux 环境注意

社区经验普遍把 Nexus + TaimiHUD 当作 Blish HUD Linux 问题的替代路线之一。常见痛点是 Blish 覆盖层透明、黑屏、输入焦点、长时间运行资源占用等。

注意：

- 不同发行版、桌面环境、Wine/Proton 版本差异明显。
- Steam 版和独立启动器路径不同。
- 先最小化安装，只启用 Nexus 和 TaimiHUD。
- 如果 TaimiHUD 的 Open Folder 按钮报错，可以手动创建 pathing / addon 数据目录，再把标记包放进去。

## 风险边界

ArenaNet 不审核、不批准、不背书任何第三方程序。一般原则是不能提供不公平优势、不能自动化、不能无人值守、不能影响其他玩家。任何第三方程序都属于自担风险。

迁移时尤其要小心：

- 宏和自动按键。
- 训练场自动配置。
- 任何可能改变即时战斗结果的工具。
- 从非官方来源下载的 DLL。

## 实用回退策略

每次改动前记录这三件事：

- GW2 目录里当前 `d3d11.dll` 是什么。
- `<GW2>/addons` 里新增了哪些 addon。
- Blish HUD 里关闭了哪些模块。

出问题时按相反顺序回退。不要一次性删除所有插件，否则很难定位是 Nexus、某个 addon、ArcDPS、ReShade 还是 Blish 共存导致的问题。
