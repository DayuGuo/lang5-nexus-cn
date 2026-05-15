# 核心差异和迁移判断

## 工作方式不同

Blish HUD 是独立程序。它用透明覆盖层盖在 Guild Wars 2 窗口上方，不把自己注入游戏进程。它主要通过 MumbleLink 获取角色位置、镜头、地图、职业等实时信息，通过 GW2 Web API 获取账号、物品、成就等数据。

Nexus 是游戏内 addon engine / addon manager。它以 `d3d11.dll` 的形式放在 GW2 安装目录，随游戏加载，通过 DirectX 11 代理方式渲染游戏内界面，并在游戏内管理 addon。

## 模块格式不同

Blish HUD：

- 加载 Blish HUD 模块，通常是 `.bhm` 包。
- 面向 .NET / C# 模块生态。
- 强项是 overlay UI、GW2 API、MumbleLink、Pathing 标记包。

Nexus：

- 加载 Nexus addon，通常是 `.dll`。
- 面向 C/C++ API，也有社区语言绑定。
- 强项是游戏内加载、热加载、addon 库、自动更新、统一快捷键、ArcDPS 等游戏内工具整合。

## 不能直接迁移的内容

这些内容不能直接从 Blish 复制到 Nexus 使用：

- Blish HUD 的 `.bhm` 模块。
- Blish 模块设置文件。
- Blish 的 API key 授权状态。
- 依赖 Blish UI 框架的模块。

这些内容可能可以复用或部分复用：

- TacO/Blish 格式的标记包，取决于 Nexus 侧 addon 的兼容范围。
- ArcDPS 日志目录。
- 通用的外部数据源，例如 GW2 Wiki、dps.report、gw2wingman。

## 迁移前判断

适合迁到 Nexus 为主：

- 你主要用 ArcDPS、日志上传、战斗 UI、鼠标增强、游戏内插件库。
- 你在 Linux、Steam Deck、Crossover、Bottles 等环境下遇到 Blish 覆盖层问题。
- 你希望插件都在游戏内管理，并减少额外启动一个 overlay 程序。

适合继续保留 Blish HUD：

- 你重度依赖 Blish Pathing 的高级标记包特性。
- 你使用的模块在 Nexus 里没有等价 addon。
- 你更在意低侵入式 overlay，而不是游戏内 addon。
- 你已经有稳定的 Blish 配置，不想为少量收益重配插件。
