# 常见功能对照

这张表不是完整清单，而是迁移时最常见的功能替换思路。

| Blish HUD 用途 | Nexus 侧候选 | 替代程度 | 备注 |
|---|---|---:|---|
| Pathing 路线、标记、跳跳乐、采集路线 | TaimiHUD | 中到高 | 可覆盖很多 TacO/Blish 类路线需求，但 Blish Pathing 的高级标记包特性更完整。 |
| Blish Pathing 高级标记包、Lua、专有属性 | TaimiHUD 或继续保留 Blish | 低到中 | Blish Pathing 官方称支持 20+ 独有属性和 Lua 脚本，不能默认认为 Nexus 侧完全兼容。 |
| 世界 Boss / 大型事件计时 | World Bosses、Event Timers、TaimiHUD | 中到高 | Nexus addon 库中已有世界 Boss、事件计时相关 addon。 |
| ArcDPS 相关 | ArcDPS via Nexus | 高 | Nexus 适合作为 ArcDPS 的加载和管理入口。 |
| 日志上传 | Log Uploader | 高 | Nexus addon 库有上传到 dps.report / gw2wingman 的工具。 |
| 鼠标高亮 / 战斗找鼠标 | Combat Cursor、Custom Cursors、CursorAnchor | 中 | 社区反馈 Nexus 侧组合能用，但不一定像 Blish Mouse Cursor 一样顺手。 |
| 装备/战斗文字 | Custom Combat Text | 中 | 依赖 ArcDPS 的场景要注意前置条件。 |
| 小队/指挥官辅助 | Commander's Toolkit、Squad Attendance Taker、Meta Train Commander | 中到高 | Nexus 生态偏游戏内实战工具，适合团本、车队、指挥场景。 |
| 地图完成度 | True World Completion、Map Completion tracker | 中到高 | 可替代部分 Blish 地图辅助需求。 |
| 物品详情、价格、Wiki、装饰预览 | Item Detail Popups；Blish Decor 继续保留 | 低到中 | Nexus 有物品详情弹窗；Blish Decor 这类专门模块未必有完整替代。 |
| 乐器演奏、自动按键类模块 | 暂不建议直接迁移 | 不确定 | 这类功能要特别检查 ArenaNet 宏政策和 addon 行为，避免自动化风险。 |
| API 账号信息、成就、库存、角色资料 | 视具体 Nexus addon；或保留 Blish | 不确定 | Blish 对 GW2 Web API 和模块级 subtoken 管理比较成熟。 |

## 迁移优先级建议

优先迁：

- ArcDPS
- Log Uploader
- World Bosses / Event Timers
- Combat Cursor / Custom Cursors
- Commander's Toolkit

谨慎迁：

- Pathing 高级标记包
- 复杂成就路线
- Blish 专属 API 模块
- 装饰/家园类模块

暂时保留 Blish：

- 找不到 Nexus 等价 addon 的模块
- 对标记包兼容性要求很高的路线模块
- 你已经配置复杂、且当前稳定的模块
