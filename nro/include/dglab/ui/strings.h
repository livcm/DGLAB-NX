#pragma once

// UI text, one table per language (nro/source/ui/strings.c). Screens ask for a
// key, never for a literal, so adding a language is adding a column
// (docs/nro-ui.md). Proper nouns (DGLAB, Joy-Con, sysmodule, BLE, PoC, Socket)
// stay as they are.

#include <dglab/ui/language.h>

typedef enum {
    // menu
    DglabString_MenuTitle = 0,
    DglabString_SysmoduleOk,
    DglabString_SysmoduleDown,
    DglabString_ItemSocket,
    DglabString_ItemMotion,
    DglabString_ItemAdvanced,
    DglabString_ItemAbout,
    DglabString_ItemBlePoc,
    DglabString_DescSocket,
    DglabString_DescMotion,
    DglabString_DescAdvanced,
    DglabString_DescAbout,
    DglabString_DescBlePoc,
    DglabString_MenuSelect,
    DglabString_MenuStart,

    // about
    DglabString_AboutTitle,
    DglabString_AboutLine1,
    DglabString_AboutLine2,
    DglabString_AboutSource,
    DglabString_AboutLanguage,
    DglabString_AboutLangAuto,
    DglabString_AboutLangZh,
    DglabString_AboutLangEn,
    DglabString_AboutFooter,

    // motion screen
    DglabString_MotionTitle,
    DglabString_MotionChannels,
    DglabString_MotionLink,
    DglabString_MotionVolume,
    DglabString_MotionLastCmd,
    DglabString_MotionStill,
    DglabString_MotionMoving,
    DglabString_MotionLevel,
    DglabString_MotionNotConnected,
    DglabString_MotionDesc,
    DglabString_MotionSafety,
    DglabString_MotionSleepWarning,
    DglabString_MotionClear,
    DglabString_MotionBack,
    DglabString_LinkNotStarted,
    DglabString_LinkWaiting,
    DglabString_LinkPaired,
    DglabString_LinkStopped,
    DglabString_LinkFailed,
    DglabString_LinkIpcFailed,

    // advanced screen
    DglabString_AdvancedTitle,
    DglabString_AdvancedSaved,
    DglabString_AdvancedSaveFailed,
    DglabString_AdvancedSelect,
    DglabString_AdvancedReset,

    // the motion parameters (name then description, in order)
    DglabString_SetDeadzoneEnter,
    DglabString_SetDeadzoneExit,
    DglabString_SetGyroRange,
    DglabString_SetAccelRange,
    DglabString_SetGyroWeight,
    DglabString_SetAccelWeight,
    DglabString_SetAttack,
    DglabString_SetRelease,
    DglabString_SetIdleStop,
    DglabString_SetFrequencyFast,
    DglabString_SetFrequencyStill,
    DglabString_SetStrengthMax,
    DglabString_DescDeadzoneEnter,
    DglabString_DescDeadzoneExit,
    DglabString_DescGyroRange,
    DglabString_DescAccelRange,
    DglabString_DescGyroWeight,
    DglabString_DescAccelWeight,
    DglabString_DescAttack,
    DglabString_DescRelease,
    DglabString_DescIdleStop,
    DglabString_DescFrequencyFast,
    DglabString_DescFrequencyStill,
    DglabString_DescStrengthMax,

    // socket test screen
    DglabString_SocketTitle,
    DglabString_PanelServer,
    DglabString_SocketPort,
    DglabString_LabelState,
    DglabString_LabelAddress,
    DglabString_LabelController,
    DglabString_LabelAppId,
    DglabString_LabelCounters,
    DglabString_LabelHeartbeats,
    DglabString_LabelAppReport,
    DglabString_LabelLastIssue,
    DglabString_CommandLabel,
    DglabString_LabelStrength,
    DglabString_StateNotStarted,
    DglabString_StateWaiting,
    DglabString_StateConnected,
    DglabString_StateStopped,
    DglabString_StateFailed,
    DglabString_NoReport,
    DglabString_NoAddress,
    DglabString_IssueNone,
    DglabString_QrHint,
    DglabString_QrNotRunning,
    DglabString_QrNoAddress,
    DglabString_QrTooLong,
    DglabString_LogTitle,
    DglabString_SocketKeys,
    DglabString_SocketValues,
    DglabString_SleepWarning,

    // what the buttons sent, and what came back
    DglabString_CmdStart,
    DglabString_CmdStop,
    DglabString_CmdClear,
    DglabString_CmdTestA,
    DglabString_CmdTestB,
    DglabString_CmdUpA,
    DglabString_CmdDownA,
    DglabString_CmdUpB,
    DglabString_CmdDownB,
    DglabString_CmdWaveformA,
    DglabString_CmdWaveformB,
    DglabString_CmdOk,
    DglabString_CmdNoApp,
    DglabString_CmdRejected,
    DglabString_CmdSocketError,
    DglabString_CmdChannelZeroA,
    DglabString_CmdChannelZeroB,

    DglabString_Count,
} DglabString;

/// Switches the language the lookups use (never Auto; resolve it first).
void dglabStringsSetLanguage(DglabLanguage language);

/// The text for the current language. Falls back to English, and to "" for an
/// out of range key, so a missing translation can never crash a screen.
const char* dglabString(DglabString id);

/// The text for a specific language, for tests and for the language row itself.
const char* dglabStringFor(DglabLanguage language, DglabString id);
