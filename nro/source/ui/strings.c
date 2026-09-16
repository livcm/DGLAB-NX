#include <dglab/ui/strings.h>

#include <stddef.h>

// One table per language, indexed by DglabString. Missing entries fall back to
// English (see dglabStringFor), and tests/canvas checks that neither table has
// holes - a screen showing an empty label is worse than showing English.

static const char* const kEnglish[DglabString_Count] = {
    [DglabString_MenuTitle] = "DGLAB-NX   modes",
    [DglabString_SysmoduleOk] = "sysmodule ok",
    [DglabString_SysmoduleDown] = "sysmodule not answering",
    [DglabString_ItemSocket] = "Socket test",
    [DglabString_ItemMotion] = "Motion (Joy-Con)",
    [DglabString_ItemAdvanced] = "Advanced (motion)",
    [DglabString_ItemAbout] = "About",
    [DglabString_ItemBlePoc] = "BLE PoC console",
    [DglabString_DescSocket] =
        "Start the socket server, show the QR code the DG-LAB app scans, and test both channels "
        "by hand. The server stops itself 55 s after the last app leaves.",
    [DglabString_DescMotion] =
        "Drive the waveform with the Joy-Cons: the more one moves, the stronger and denser its "
        "channel gets. Left Joy-Con is channel A, right is B. Start the socket server in Socket "
        "test first.",
    [DglabString_DescAdvanced] =
        "Every motion parameter on one page - dead zone, sensitivity, envelope, frequency and the "
        "waveform strength - edited one step at a time and saved to the SD card, so a tuning "
        "session survives a restart.",
    [DglabString_DescAbout] = "Project information, where the source lives, and the language.",
    [DglabString_DescBlePoc] =
        "Console view from the abandoned host side BLE experiments. Kept because it is the "
        "rendering path that is known to work on real hardware.",
    [DglabString_MenuSelect] = "D-pad up/down select",
    [DglabString_MenuStart] = "A start mode    + exit",

    [DglabString_AboutTitle] = "DGLAB-NX   about",
    [DglabString_AboutLine1] = "Unofficial DG-LAB controller for Nintendo Switch.",
    [DglabString_AboutLine2] =
        "The socket server and the protocol live in the sysmodule; this front end talks to it "
        "over IPC only.",
    [DglabString_AboutSource] = "Source code and releases:",
    [DglabString_AboutLanguage] = "Language",
    [DglabString_AboutLangAuto] = "follow the console",
    [DglabString_AboutLangZh] = "Simplified Chinese",
    [DglabString_AboutLangEn] = "English",
    [DglabString_AboutFooter] = "left/right change    + back",

    [DglabString_MotionTitle] = "DGLAB-NX   motion (Joy-Con)",
    [DglabString_MotionChannels] = "channels",
    [DglabString_MotionLink] = "link",
    [DglabString_MotionVolume] = "channel strength",
    [DglabString_MotionLastCmd] = "last cmd",
    [DglabString_MotionStill] = "still",
    [DglabString_MotionMoving] = "moving",
    [DglabString_MotionLevel] = "waveform",
    [DglabString_MotionNotConnected] = "not connected",
    [DglabString_MotionDesc] =
        "Move a Joy-Con: the harder it is moved, the stronger and denser its channel becomes. "
        "Both fall back to silence when it is still.",
    [DglabString_MotionSafety] =
        "This mode sets the waveform only: the channel strength above is the volume it is scaled "
        "by, and that is set in Socket test. The device really does output current.",
    [DglabString_MotionSleepWarning] =
        "Do not sleep while the server holds its socket: press Y in Socket test first.",
    [DglabString_MotionClear] = "B clear both channels",
    [DglabString_MotionBack] = "+ back to the menu",
    [DglabString_LinkNotStarted] = "server not started",
    [DglabString_LinkWaiting] = "waiting for the app",
    [DglabString_LinkPaired] = "app connected",
    [DglabString_LinkStopped] = "server stopped",
    [DglabString_LinkFailed] = "server failed",
    [DglabString_LinkIpcFailed] = "IPC call failed",

    [DglabString_AdvancedTitle] = "DGLAB-NX   advanced (motion)",
    [DglabString_AdvancedSaved] = "saved to sdmc:/switch/DGLAB-NX/motion.cfg",
    [DglabString_AdvancedSaveFailed] = "could not write motion.cfg (settings still apply now)",
    [DglabString_AdvancedSelect] =
        "D-pad up/down select    left/right change: one step per press",
    [DglabString_AdvancedReset] = "Y reset to the defaults    + back to the menu",

    [DglabString_SetDeadzoneEnter] = "deadzone enter",
    [DglabString_SetDeadzoneExit] = "deadzone exit",
    [DglabString_SetGyroRange] = "gyro range",
    [DglabString_SetAccelRange] = "accel range",
    [DglabString_SetGyroWeight] = "gyro weight",
    [DglabString_SetAccelWeight] = "accel weight",
    [DglabString_SetAttack] = "attack",
    [DglabString_SetRelease] = "release",
    [DglabString_SetIdleStop] = "idle stop",
    [DglabString_SetFrequencyFast] = "freq fast",
    [DglabString_SetFrequencyStill] = "freq still",
    [DglabString_SetStrengthMax] = "strength max",
    [DglabString_DescDeadzoneEnter] =
        "How much movement is needed before anything is output. Raise it if a hand that merely "
        "holds a Joy-Con still produces output.",
    [DglabString_DescDeadzoneExit] =
        "How far the movement has to fall before the mode counts as still again. It sits below "
        "the enter value so the level does not flicker at the edge.",
    [DglabString_DescGyroRange] =
        "The rotation speed that counts as full output. Smaller means a gentler movement already "
        "reaches the top.",
    [DglabString_DescAccelRange] =
        "The acceleration change that counts as full output: this is the term that reacts to a "
        "sudden jerk rather than to steady movement.",
    [DglabString_DescGyroWeight] =
        "How much this term contributes. Set one to 0 to find out what the other one is "
        "responsible for.",
    [DglabString_DescAccelWeight] =
        "How much this term contributes. Set one to 0 to find out what the other one is "
        "responsible for.",
    [DglabString_DescAttack] =
        "How quickly the output follows a movement. Too short and every reading shows up as a "
        "spike.",
    [DglabString_DescRelease] =
        "How long the output takes to fall back to silence after the movement stops.",
    [DglabString_DescIdleStop] =
        "How long the controller has to stay still before uploading stops completely. The "
        "release still finishes first.",
    [DglabString_DescFrequencyFast] =
        "The pulse interval at full intensity. Smaller is denser: more pulses per second at the "
        "same amplitude is also more current, so this is a strength change too. The device floor "
        "is 10ms.",
    [DglabString_DescFrequencyStill] =
        "The pulse interval while still. This is what the output starts from as a movement fades "
        "out.",
    [DglabString_DescStrengthMax] =
        "The waveform strength at full intensity, on top of the channel strength set in Socket "
        "test. The device multiplies the two.",

    [DglabString_SocketTitle] = "DGLAB-NX   socket server",
    [DglabString_SocketPort] = "port %u",
    [DglabString_LabelState] = "state",
    [DglabString_LabelAddress] = "address",
    [DglabString_LabelController] = "controller",
    [DglabString_LabelAppId] = "app id",
    [DglabString_LabelCounters] = "traffic",
    [DglabString_LabelHeartbeats] = "heartbeats",
    [DglabString_LabelAppReport] = "app report",
    [DglabString_LabelLastIssue] = "last issue",
    [DglabString_LabelStrength] = "strength",
    [DglabString_StateNotStarted] = "not started",
    [DglabString_StateWaiting] = "waiting for the app",
    [DglabString_StateConnected] = "app connected",
    [DglabString_StateStopped] = "stopped",
    [DglabString_StateFailed] = "FAILED",
    [DglabString_NoReport] = "none",
    [DglabString_NoAddress] = "no address yet",
    [DglabString_IssueNone] = "none",
    [DglabString_QrHint] = "scan this with the DG-LAB app",
    [DglabString_QrNotRunning] =
        "The socket server is not running. Press A to start it (it stops itself 55 s after the "
        "last app leaves).",
    [DglabString_QrNoAddress] =
        "No QR code yet: the console has no LAN address. Join the same Wi-Fi network as the phone.",
    [DglabString_QrTooLong] = "the socket url does not fit a QR code",
    [DglabString_LogTitle] = "sysmodule log",
    [DglabString_SocketKeys] =
        "A start    Y stop    B clear    ZL test ch A    ZR test ch B",
    [DglabString_SocketValues] =
        "D-pad up/down ch A    left/right ch B    + back to the menu",
    [DglabString_SleepWarning] = "do not sleep while the server runs: press Y first",

    [DglabString_CmdStart] = "start",
    [DglabString_CmdStop] = "stop",
    [DglabString_CmdClear] = "clear",
    [DglabString_CmdTestA] = "A test",
    [DglabString_CmdTestB] = "B test",
    [DglabString_CmdUpA] = "A up",
    [DglabString_CmdDownA] = "A down",
    [DglabString_CmdUpB] = "B up",
    [DglabString_CmdDownB] = "B down",
    [DglabString_CmdWaveformA] = "waveform A",
    [DglabString_CmdWaveformB] = "waveform B",
    [DglabString_CmdOk] = "ok",
    [DglabString_CmdNoApp] = "no app bound",
    [DglabString_CmdRejected] = "rejected",
    [DglabString_CmdSocketError] = "socket error",
    [DglabString_CmdChannelZeroA] = "A is 0",
    [DglabString_CmdChannelZeroB] = "B is 0",
};

static const char* const kChinese[DglabString_Count] = {
    [DglabString_MenuTitle] = "DGLAB-NX   模式",
    [DglabString_SysmoduleOk] = "sysmodule 正常",
    [DglabString_SysmoduleDown] = "sysmodule 无响应",
    [DglabString_ItemSocket] = "连接测试",
    [DglabString_ItemMotion] = "体感（Joy-Con）",
    [DglabString_ItemAdvanced] = "高级参数",
    [DglabString_ItemAbout] = "关于",
    [DglabString_ItemBlePoc] = "BLE PoC 控制台",
    [DglabString_DescSocket] =
        "启动 Socket 服务端，显示 DG-LAB App 扫描的二维码，并手动测试两个通道。最后一个客户端"
        "离开 55 秒后服务端自动停止。",
    [DglabString_DescMotion] =
        "用 Joy-Con 驱动波形：挥动得越猛，对应通道的波形值越大、脉冲越密。左 Joy-Con 对应 A "
        "通道，右对应 B 通道。请先在「连接测试」里启动服务端。",
    [DglabString_DescAdvanced] =
        "体感玩法的全部参数都在这一页——死区、灵敏度、包络、频率和波形强度上限。一次改一格，"
        "改动保存到 SD 卡，重启后仍然有效。",
    [DglabString_DescAbout] = "项目信息、源码地址，以及界面语言。",
    [DglabString_DescBlePoc] = "主机侧 BLE 直连实验留下的控制台视图，保留用于诊断。",
    [DglabString_MenuSelect] = "十字键 上下选择",
    [DglabString_MenuStart] = "A 进入    + 退出",

    [DglabString_AboutTitle] = "DGLAB-NX   关于",
    [DglabString_AboutLine1] = "非官方 DG-LAB 控制器实现，运行在 Nintendo Switch 上。",
    [DglabString_AboutLine2] = "Socket 服务端与协议都在 sysmodule 里，这个前端只通过 IPC 与它通信。",
    [DglabString_AboutSource] = "源码与发布：",
    [DglabString_AboutLanguage] = "语言",
    [DglabString_AboutLangAuto] = "跟随系统",
    [DglabString_AboutLangZh] = "简体中文",
    [DglabString_AboutLangEn] = "English",
    [DglabString_AboutFooter] = "左右修改    + 返回",

    [DglabString_MotionTitle] = "DGLAB-NX   体感（Joy-Con）",
    [DglabString_MotionChannels] = "通道",
    [DglabString_MotionLink] = "连接",
    [DglabString_MotionVolume] = "通道强度",
    [DglabString_MotionLastCmd] = "最近指令",
    [DglabString_MotionStill] = "静止",
    [DglabString_MotionMoving] = "挥动中",
    [DglabString_MotionLevel] = "波形值",
    [DglabString_MotionNotConnected] = "未连接",
    [DglabString_MotionDesc] = "挥动 Joy-Con：动得越猛，该通道的波形值越大、脉冲越密；停下来后会平滑回落。",
    [DglabString_MotionSafety] =
        "本模式只改波形值：上面的「通道强度」是音量，在「连接测试」里设置。设备会真实输出电流。",
    [DglabString_MotionSleepWarning] =
        "服务端持有 socket 时不要让主机休眠：先在「连接测试」按 Y 停止。",
    [DglabString_MotionClear] = "B 清空两个通道",
    [DglabString_MotionBack] = "+ 返回菜单",
    [DglabString_LinkNotStarted] = "服务端未启动",
    [DglabString_LinkWaiting] = "等待 App",
    [DglabString_LinkPaired] = "已连接 App",
    [DglabString_LinkStopped] = "服务端已停止",
    [DglabString_LinkFailed] = "服务端失败",
    [DglabString_LinkIpcFailed] = "IPC 调用失败",

    [DglabString_AdvancedTitle] = "DGLAB-NX   高级参数（体感）",
    [DglabString_AdvancedSaved] = "已保存到 sdmc:/switch/DGLAB-NX/motion.cfg",
    [DglabString_AdvancedSaveFailed] = "无法写入 motion.cfg（设置对本次运行仍然有效）",
    [DglabString_AdvancedSelect] = "十字键 上下选择　　左右修改：按一下一格",
    [DglabString_AdvancedReset] = "Y 恢复默认　　+ 返回菜单",

    [DglabString_SetDeadzoneEnter] = "死区进入",
    [DglabString_SetDeadzoneExit] = "死区退出",
    [DglabString_SetGyroRange] = "陀螺仪量程",
    [DglabString_SetAccelRange] = "加速度量程",
    [DglabString_SetGyroWeight] = "陀螺仪权重",
    [DglabString_SetAccelWeight] = "加速度权重",
    [DglabString_SetAttack] = "上升时间",
    [DglabString_SetRelease] = "释放时间",
    [DglabString_SetIdleStop] = "静止停发延时",
    [DglabString_SetFrequencyFast] = "最快频率",
    [DglabString_SetFrequencyStill] = "静止频率",
    [DglabString_SetStrengthMax] = "波形强度上限",
    [DglabString_DescDeadzoneEnter] =
        "需要多大的动作才开始有输出。握在手里不动也会有输出时，把它调大。",
    [DglabString_DescDeadzoneExit] =
        "动作回落到多小才算重新静止。它比进入值更小，是为了避免在阈值附近来回跳。",
    [DglabString_DescGyroRange] =
        "算作满输出所需的旋转速度。调小则轻轻一动就能到顶。",
    [DglabString_DescAccelRange] =
        "算作满输出所需的加速度变化量，负责「突然一顿」这类冲击，而不是匀速移动。",
    [DglabString_DescGyroWeight] = "这一项占多大比重。把其中一项设成 0，就能看清另一项在做什么。",
    [DglabString_DescAccelWeight] = "这一项占多大比重。把其中一项设成 0，就能看清另一项在做什么。",
    [DglabString_DescAttack] = "输出跟随动作的速度。太快的话每一条读数都会变成一次尖峰。",
    [DglabString_DescRelease] = "动作停下后，输出回落到静音所需的时间。",
    [DglabString_DescIdleStop] =
        "静止多久之后完全停止上传。释放曲线会先走完，再停。",
    [DglabString_DescFrequencyFast] =
        "满强度时的脉冲间隔。越小越密：同样幅度下每秒的脉冲更多，送出的电流也更多，所以它同时"
        "改变强度和手感。设备下限 10ms。",
    [DglabString_DescFrequencyStill] = "静止时的脉冲间隔，也就是动作淡出后输出的起点。",
    [DglabString_DescStrengthMax] =
        "满强度时的波形值，在「连接测试」里设的通道强度之上。设备会把两者相乘。",

    [DglabString_SocketTitle] = "DGLAB-NX   Socket 服务端",
    [DglabString_SocketPort] = "端口 %u",
    [DglabString_LabelState] = "状态",
    [DglabString_LabelAddress] = "地址",
    [DglabString_LabelController] = "控制端",
    [DglabString_LabelAppId] = "App 标识",
    [DglabString_LabelCounters] = "流量",
    [DglabString_LabelHeartbeats] = "心跳",
    [DglabString_LabelAppReport] = "App 上报",
    [DglabString_LabelLastIssue] = "最近错误",
    [DglabString_LabelStrength] = "通道强度",
    [DglabString_StateNotStarted] = "未启动",
    [DglabString_StateWaiting] = "等待 App",
    [DglabString_StateConnected] = "已连接 App",
    [DglabString_StateStopped] = "已停止",
    [DglabString_StateFailed] = "启动失败",
    [DglabString_NoReport] = "无",
    [DglabString_NoAddress] = "暂无地址",
    [DglabString_IssueNone] = "无",
    [DglabString_QrHint] = "用 DG-LAB App 扫码",
    [DglabString_QrNotRunning] =
        "Socket 服务端未运行。按 A 启动（最后一个客户端离开 55 秒后会自动停止）。",
    [DglabString_QrNoAddress] = "还没有二维码：主机没有局域网地址。请把手机连到同一个 Wi-Fi。",
    [DglabString_QrTooLong] = "地址太长，放不进二维码",
    [DglabString_LogTitle] = "sysmodule 日志",
    [DglabString_SocketKeys] = "A 启动    Y 停止    B 清空    ZL 测 A    ZR 测 B",
    [DglabString_SocketValues] = "十字键 上下调 A    左右调 B    + 返回菜单",
    [DglabString_SleepWarning] = "服务端运行时不要休眠：先按 Y 停止",

    [DglabString_CmdStart] = "启动",
    [DglabString_CmdStop] = "停止",
    [DglabString_CmdClear] = "清空",
    [DglabString_CmdTestA] = "A 测试",
    [DglabString_CmdTestB] = "B 测试",
    [DglabString_CmdUpA] = "A 加",
    [DglabString_CmdDownA] = "A 减",
    [DglabString_CmdUpB] = "B 加",
    [DglabString_CmdDownB] = "B 减",
    [DglabString_CmdWaveformA] = "波形 A",
    [DglabString_CmdWaveformB] = "波形 B",
    [DglabString_CmdOk] = "正常",
    [DglabString_CmdNoApp] = "没有绑定 App",
    [DglabString_CmdRejected] = "被拒绝",
    [DglabString_CmdSocketError] = "socket 错误",
    [DglabString_CmdChannelZeroA] = "A 为 0",
    [DglabString_CmdChannelZeroB] = "B 为 0",
};

static DglabLanguage g_language = DglabLanguage_English;

void dglabStringsSetLanguage(DglabLanguage language)
{
    if (language == DglabLanguage_ChineseSimplified)
        g_language = language;
    else
        g_language = DglabLanguage_English;
}

const char* dglabStringFor(DglabLanguage language, DglabString id)
{
    const char* text = NULL;

    if (id < 0 || id >= DglabString_Count)
        return "";

    if (language == DglabLanguage_ChineseSimplified)
        text = kChinese[id];

    if (!text)
        text = kEnglish[id];

    return text ? text : "";
}

const char* dglabString(DglabString id)
{
    return dglabStringFor(g_language, id);
}
