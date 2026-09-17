#pragma once

// UI text. Screens ask for a key, never for a literal: the text itself lives in
// one .json file per language, next to the NRO, and is loaded at startup
// (nro/source/ui/strings.c, docs/nro-ui.md). Proper nouns (DGLAB, Joy-Con,
// sysmodule, BLE, PoC, Socket) stay as they are in every language.

#include <dglab/ui/language.h>

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    // The bottom bar. These are actions, never button names: the button is
    // drawn as its own icon (dglab/ui/button.h), so "B 返回" is an icon and a
    // word, and the word is the only half that can be translated.
    DglabString_ActionEnter = 0,
    DglabString_ActionBack,
    DglabString_ActionExit,
    DglabString_ActionStart,
    DglabString_ActionStop,
    DglabString_ActionClear,
    DglabString_ActionLog,     ///< open the sysmodule log page
    DglabString_ActionClose,   ///< and close it again
    DglabString_ActionTestChannels, ///< one hint, the ZL and ZR icons
    DglabString_ActionAdjust,
    DglabString_ActionLanguage,
    DglabString_ActionTheme,   ///< the About page's Y
    DglabString_ActionReset,
    DglabString_ActionRescan, ///< take the Joy-Con sensor handles again
    // the two adjustment hints the pages put inside their own content
    DglabString_HintAdjustA,
    DglabString_HintAdjustB,

    // menu: the only page whose title carries the app's name
    DglabString_MenuTitle,
    DglabString_SysmoduleOk,
    DglabString_SysmoduleDown,
    // The menu's entries are the pages' own titles (see menu.c): one string per
    // page, so a list entry and the page it opens can never drift apart. The BLE
    // PoC console is the exception - it has no page of its own in this UI.
    DglabString_ItemBlePoc,
    DglabString_DescSocket,
    DglabString_DescMotion,
    DglabString_DescAdvanced,
    DglabString_DescAbout,
    DglabString_DescBlePoc,

    // about
    DglabString_AboutTitle,
    DglabString_AboutLine1,
    DglabString_AboutLine2,
    // Two versions sit on this page and they are different numbers: this NRO's
    // own release version (the repository's VERSION) and the sysmodule's IPC
    // interface version (docs/ipc.md, "版本"). Each has its own row and its label
    // has to say which one it is - the page's header carries the sysmodule state
    // like every other page, not a version.
    DglabString_AboutAppVersion,
    DglabString_AboutIpcVersion,
    DglabString_AboutBuild,
    DglabString_AboutSource,
    DglabString_AboutLanguage,
    DglabString_AboutLangAuto,
    DglabString_AboutLangZh,
    DglabString_AboutLangEn,
    // The colour theme row, right under the language one: the preference is the
    // same three way shape (follow the console / light / dark), so its values
    // read the same way - Auto says what the console is, in brackets.
    DglabString_AboutTheme,
    DglabString_AboutThemeAuto,
    DglabString_AboutThemeLight,
    DglabString_AboutThemeDark,

    // motion screen
    DglabString_MotionTitle,
    DglabString_MotionLink,
    DglabString_MotionVolume,
    // The motion page's input rows: which side's Joy-Con is being read, and how
    // hard it is being moved. They are not the DG-LAB channels - those are the
    // strength rows below them (docs/joycon-input.md).
    DglabString_MotionJoyConLeft,
    DglabString_MotionJoyConRight,
    DglabString_MotionStill,
    DglabString_MotionMoving,
    DglabString_MotionLevel,
    DglabString_MotionNotConnected,

    // advanced screen
    DglabString_AdvancedTitle,
    DglabString_AdvancedSaved,
    DglabString_AdvancedSaveFailed,

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
    // The two weight terms share one sentence, so they share one key.
    DglabString_DescWeight,
    DglabString_DescAttack,
    DglabString_DescRelease,
    DglabString_DescIdleStop,
    DglabString_DescFrequencyFast,
    DglabString_DescFrequencyStill,
    DglabString_DescStrengthMax,

    // socket test screen
    DglabString_SocketTitle,
    DglabString_RowServer,
    DglabString_LabelChannelA,
    DglabString_LabelChannelB,
    DglabString_LabelAddress,
    DglabString_LabelAppId,
    DglabString_LabelCounters,
    DglabString_LabelHeartbeats,
    DglabString_LabelAppReport,
    DglabString_CommandLabel,
    // The server's state, shared by the socket page's server row and the motion
    // page's link line so the two can never say different things about the same
    // state (dglabNetStateText()). IpcFailed is the state of the call itself:
    // the sysmodule did not answer at all.
    DglabString_StateNotStarted,
    DglabString_StateWaiting,
    DglabString_StateConnected,
    DglabString_StateStopped,
    DglabString_StateFailed,
    DglabString_StateIpcFailed,
    DglabString_NoReport,
    DglabString_NoAddress,
    DglabString_QrHint,
    DglabString_QrNotRunning,
    DglabString_QrTooLong,
    DglabString_LogTitle,
    DglabString_SleepWarning,
    DglabString_SleepWarningAutoOff,

    // what the buttons sent, and what came back
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

/// Longest value a language file may carry. The longest line the screens use is
/// a description of about 200 bytes, so this only ever catches a file that is
/// not a language file at all.
#define DGLAB_STRING_VALUE_MAX 512

/// Longest key (member name inside "strings").
#define DGLAB_STRING_KEY_MAX 64

/// What loading one file did.
typedef enum {
    DglabStringsLoad_Ok = 0,   ///< every key came from the file
    DglabStringsLoad_Incomplete, ///< usable, but something about it is off
    DglabStringsLoad_Failed,     ///< nothing was taken from this file
} DglabStringsLoadResult;

/// The first thing that was wrong with a file. Only the kinds the loader can
/// name itself; a syntax mistake is reported with the reader's message.
typedef enum {
    DglabStringsProblem_None = 0,
    DglabStringsProblem_Syntax,       ///< not valid JSON, see line
    DglabStringsProblem_Structure,    ///< valid JSON, but not a language file
    DglabStringsProblem_Language,     ///< the "language" field is not this file's code
    DglabStringsProblem_MissingKeys,  ///< the file goes without some of the keys
    DglabStringsProblem_UnknownKeys,  ///< the file carries keys the UI does not ask for
    DglabStringsProblem_DuplicateKey, ///< the same key twice
    DglabStringsProblem_OutOfMemory,
    DglabStringsProblem_Unsupported, ///< not one of the languages that have files
} DglabStringsProblem;

typedef struct {
    DglabStringsProblem problem; ///< the first thing that went wrong
    unsigned line; ///< 1 based, set for a syntax mistake
    unsigned count; ///< how many keys `problem` covers, 0 when it is not counted
    unsigned missing; ///< keys the file does not carry at all
    unsigned unknown; ///< keys the file carries that the UI never asks for
    char detail[96]; ///< first missing or unknown key, or the reader's message
    char language[16]; ///< the "language" field the file declared, "" when absent
} DglabStringsReport;

/// Reads one language out of the text of its .json file. `report` is optional
/// and is written for every call, including the successful ones. A failed load
/// leaves the language table exactly as it was.
DglabStringsLoadResult dglabStringsLoadJson(DglabLanguage language, const char* text, size_t size,
    DglabStringsReport* report);

/// Drops every loaded language. For tests: the NRO loads once and keeps it.
void dglabStringsReset(void);

/// Switches the language the lookups use (never Auto; resolve it first).
void dglabStringsSetLanguage(DglabLanguage language);

/// The text for the current language. Falls back to another loaded language,
/// and to "" for an out of range key, so a language file that is missing an
/// entry can never crash a screen.
const char* dglabString(DglabString id);

/// The text for a specific language, for tests and for the language row itself.
const char* dglabStringFor(DglabLanguage language, DglabString id);

/// The key `id` uses in the language files, e.g. "motion_title"; NULL when the
/// id is out of range. Used by the loader and by tests.
const char* dglabStringKeyName(DglabString id);
