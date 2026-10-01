# Game Mod 工作规则

`mods/<game>/` 负责事件检测、Hook、读取游戏状态，并将事件映射到 sysmodule IPC。
不自行连接 DG-LAB 或实现底层协议；遵循 [根规则](../AGENTS.md)。

每个 Mod 必须在对应目录文档（或 `docs/game-mods.md`）记录 Title ID、游戏版本、
适用的 Build ID、Hook 地址/函数、相关内存结构、已验证及不兼容版本；代码也要注明适用版本。
禁止假设不同游戏版本的地址、函数签名和内存布局相同。
