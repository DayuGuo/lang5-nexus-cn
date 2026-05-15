# Blish HUD 迁移到 Nexus 经验整理

整理日期：2026-05-15

## 先说结论

Blish HUD 到 Nexus 不是传统意义上的“迁移”。Blish HUD 的模块是 Blish 自己加载的 `.bhm` / .NET 模块；Nexus 加载的是 Nexus/Raidcore 生态里的游戏内 addon。两边模块不能直接互相导入。

实际可行路线是：

1. 盘点你在 Blish HUD 里真正使用的模块。
2. 在 Nexus Addon Library 里找同类功能的 addon。
3. 对路径/标记、事件计时、鼠标增强、ArcDPS、日志上传等功能逐项替换。
4. 保留 Blish HUD 负责 Nexus 暂时没有等价功能的模块。
5. 稳定后再决定是否完全停用 Blish HUD。

## 最推荐的迁移策略

优先采用“共存过渡”，不要直接卸载 Blish HUD。

第一阶段：安装 Nexus，只迁移 ArcDPS、日志上传、鼠标增强、世界 Boss / 事件计时这类成熟 addon。

第二阶段：如果主要依赖 Blish 的 Pathing 路径模块，尝试 Nexus 生态里的 TaimiHUD。它面向 TacO/Blish HUD 类功能，支持路径、标记、计时等方向，但和 Blish Pathing 不是完全等价。

第三阶段：对照自己的模块清单，把无法替代的 Blish 模块继续保留。比如部分账号/API 工具、装饰预览、演奏/乐器、复杂标记包特性，可能仍然更适合留在 Blish HUD。

## 目录

- [01-core-differences.md](01-core-differences.md)：核心差异和迁移判断
- [02-install-coexist-rollback.md](02-install-coexist-rollback.md)：安装、共存、回退步骤
- [03-module-mapping.md](03-module-mapping.md)：常见 Blish 功能到 Nexus addon 的对照
- [04-troubleshooting-notes.md](04-troubleshooting-notes.md)：排错、风险和经验
- [module-inventory-template.csv](module-inventory-template.csv)：迁移盘点表模板
- [sources.md](sources.md)：资料来源

## 一句话建议

如果你用 Blish HUD 主要是路线、成就、采集、事件提示，可以先不要急着迁。Nexus + TaimiHUD 可以替代一部分，但 Blish Pathing 的标记包兼容和高级特性仍然更完整。

如果你用 Blish HUD 只是顺带开着，而主要需求是 ArcDPS、插件管理、游戏内 addon、Linux/Steam Deck 兼容、减少外部覆盖层问题，那么 Nexus 更适合作为主插件平台。
