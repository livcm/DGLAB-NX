# Game Mods AGENTS.md

## 职责

`mods/` 存放特定游戏的联动代码。

Game Mod 负责：

- 游戏事件检测；
- Hook；
- 游戏内状态读取；
- 根据游戏事件调用 Sysmodule IPC。

## 交互模型

    玩家受到伤害
        ↓
    Game Mod 检测事件
        ↓
    IPC: SendEffect(...)
        ↓
    Sysmodule
        ↓
    DG-LAB

## 边界

- Game Mod 不应该重新实现 DG-LAB Bluetooth Protocol。
- Game Mod 不得直接持有 DG-LAB BLE 连接。
- Game Mod 只负责游戏事件到 Sysmodule IPC 的映射。

## 版本信息

每个游戏 Mod 必须明确记录：

- Title ID；
- 游戏版本；
- Build ID（如果适用）；
- Hook 地址或函数；
- 相关内存结构；
- 已验证的游戏版本；
- 不兼容的游戏版本。

绝不能假设不同游戏版本之间的地址、函数签名或内存布局保持不变。

版本敏感内容应优先记录在对应 `mods/<game>/` 目录，或 `docs/game-mods.md`
中，并在代码中明确适用版本。
