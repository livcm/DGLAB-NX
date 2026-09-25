#include <dglab/ui/strings.h>

#include <dglab/util/json.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The text itself lives in lang/<code>.json, next to the NRO on the SD card, so
// a translation is a text file edit rather than a rebuild (docs/nro-ui.md).
// This file holds the other half of the contract: the key of every DglabString,
// and the loader that fills the tables out of a file the platform layer read.

// The keys the language files use. The order of the table does not matter -
// every entry is named - so a key can be added anywhere in strings.h and only
// needs its line here plus its text in every lang/*.json.
static const char* const kKeys[DglabString_Count] = {
    // the bottom bar's actions (the button itself is an icon, not text)
    [DglabString_ActionEnter] = "action_enter",
    [DglabString_ActionBack] = "action_back",
    [DglabString_ActionExit] = "action_exit",
    [DglabString_ActionStart] = "action_start",
    [DglabString_ActionStop] = "action_stop",
    [DglabString_ActionClear] = "action_clear",
    [DglabString_ActionLog] = "action_log",
    [DglabString_ActionClose] = "action_close",
    [DglabString_ActionTestChannels] = "action_test_channels",
    [DglabString_ActionAdjust] = "action_adjust",
    [DglabString_ActionLanguage] = "action_language",
    [DglabString_ActionTheme] = "action_theme",
    [DglabString_ActionReset] = "action_reset",
    [DglabString_ActionRescan] = "action_rescan",
    [DglabString_HintAdjustA] = "hint_adjust_a",
    [DglabString_HintAdjustB] = "hint_adjust_b",

    // menu
    [DglabString_MenuTitle] = "menu_title",
    [DglabString_SysmoduleOk] = "sysmodule_ok",
    [DglabString_SysmoduleDown] = "sysmodule_down",
    [DglabString_ItemBlePoc] = "item_ble_poc",
    [DglabString_BleTitle] = "ble_title",
    [DglabString_DescBle] = "desc_ble",
    [DglabString_BleState] = "ble_state",
    [DglabString_BleStateIdle] = "ble_state_idle",
    [DglabString_BleStateConnecting] = "ble_state_connecting",
    [DglabString_BleStateConnected] = "ble_state_connected",
    [DglabString_BleStateFailed] = "ble_state_failed",
    [DglabString_BleDevice] = "ble_device",
    [DglabString_BleDeviceNone] = "ble_device_none",
    [DglabString_BlePackets] = "ble_packets",
    [DglabString_BleLogTitle] = "ble_log_title",
    [DglabString_DescSocket] = "desc_socket",
    [DglabString_DescMotion] = "desc_motion",
    [DglabString_DescTouch] = "desc_touch",
    [DglabString_DescAdvanced] = "desc_advanced",
    [DglabString_DescAbout] = "desc_about",
    [DglabString_DescBlePoc] = "desc_ble_poc",

    // about
    [DglabString_AboutTitle] = "about_title",
    [DglabString_AboutLine1] = "about_line1",
    [DglabString_AboutLine2] = "about_line2",
    [DglabString_AboutAppVersion] = "about_app_version",
    [DglabString_AboutIpcVersion] = "about_ipc_version",
    [DglabString_AboutBuild] = "about_build",
    [DglabString_AboutSource] = "about_source",
    [DglabString_AboutLanguage] = "about_language",
    [DglabString_AboutLangAuto] = "about_lang_auto",
    [DglabString_AboutLangZh] = "about_lang_zh",
    [DglabString_AboutLangEn] = "about_lang_en",
    [DglabString_AboutTheme] = "about_theme",
    [DglabString_AboutThemeAuto] = "about_theme_auto",
    [DglabString_AboutThemeLight] = "about_theme_light",
    [DglabString_AboutThemeDark] = "about_theme_dark",

    // motion screen
    [DglabString_MotionTitle] = "motion_title",
    [DglabString_MotionLink] = "motion_link",
    [DglabString_MotionVolume] = "motion_volume",
    [DglabString_MotionJoyConLeft] = "motion_joycon_left",
    [DglabString_MotionJoyConRight] = "motion_joycon_right",
    [DglabString_MotionStill] = "motion_still",
    [DglabString_MotionMoving] = "motion_moving",
    [DglabString_MotionLevel] = "motion_level",
    [DglabString_MotionNotConnected] = "motion_not_connected",

    // touch screen
    [DglabString_TouchTitle] = "touch_title",
    [DglabString_TouchHalfLeft] = "touch_half_left",
    [DglabString_TouchHalfRight] = "touch_half_right",
    [DglabString_TouchNotTouched] = "touch_not_touched",
    [DglabString_TouchDensity] = "touch_density",
    [DglabString_TouchDocked] = "touch_docked",

    // advanced screen
    [DglabString_AdvancedTitle] = "advanced_title",
    [DglabString_AdvancedSaved] = "advanced_saved",
    [DglabString_AdvancedSaveFailed] = "advanced_save_failed",

    // the motion parameters (name then description, in order)
    [DglabString_SetDeadzoneEnter] = "set_deadzone_enter",
    [DglabString_SetDeadzoneExit] = "set_deadzone_exit",
    [DglabString_SetGyroRange] = "set_gyro_range",
    [DglabString_SetAccelRange] = "set_accel_range",
    [DglabString_SetGyroWeight] = "set_gyro_weight",
    [DglabString_SetAccelWeight] = "set_accel_weight",
    [DglabString_SetAttack] = "set_attack",
    [DglabString_SetRelease] = "set_release",
    [DglabString_SetIdleStop] = "set_idle_stop",
    [DglabString_SetFrequencyFast] = "set_frequency_fast",
    [DglabString_SetFrequencyStill] = "set_frequency_still",
    [DglabString_SetDensityFixed] = "set_density_fixed",
    [DglabString_SetFrequencyFixed] = "set_frequency_fixed",
    [DglabString_SetStrengthMax] = "set_strength_max",
    [DglabString_SetChannelLimitA] = "set_channel_limit_a",
    [DglabString_SetChannelLimitB] = "set_channel_limit_b",
    [DglabString_DescDeadzoneEnter] = "desc_deadzone_enter",
    [DglabString_DescDeadzoneExit] = "desc_deadzone_exit",
    [DglabString_DescGyroRange] = "desc_gyro_range",
    [DglabString_DescAccelRange] = "desc_accel_range",
    [DglabString_DescWeight] = "desc_weight",
    [DglabString_DescAttack] = "desc_attack",
    [DglabString_DescRelease] = "desc_release",
    [DglabString_DescIdleStop] = "desc_idle_stop",
    [DglabString_DescFrequencyFast] = "desc_frequency_fast",
    [DglabString_DescFrequencyStill] = "desc_frequency_still",
    [DglabString_DescDensityFixed] = "desc_density_fixed",
    [DglabString_DescFrequencyFixed] = "desc_frequency_fixed",
    [DglabString_DescStrengthMax] = "desc_strength_max",
    [DglabString_DescChannelLimit] = "desc_channel_limit",
    [DglabString_DensityFixedValue] = "density_fixed_value",
    [DglabString_DensityVariableValue] = "density_variable_value",

    // socket test screen
    [DglabString_SocketTitle] = "socket_title",
    [DglabString_RowServer] = "row_server",
    [DglabString_LabelChannelA] = "label_channel_a",
    [DglabString_LabelChannelB] = "label_channel_b",
    [DglabString_LabelAddress] = "label_address",
    [DglabString_LabelAppId] = "label_app_id",
    [DglabString_LabelCounters] = "label_counters",
    [DglabString_LabelHeartbeats] = "label_heartbeats",
    [DglabString_LabelAppReport] = "label_app_report",
    [DglabString_CommandLabel] = "command_label",
    [DglabString_StateNotStarted] = "state_not_started",
    [DglabString_StateWaiting] = "state_waiting",
    [DglabString_StateConnected] = "state_connected",
    [DglabString_StateStopped] = "state_stopped",
    [DglabString_StateFailed] = "state_failed",
    [DglabString_StateIpcFailed] = "state_ipc_failed",
    [DglabString_NoReport] = "no_report",
    [DglabString_NoAddress] = "no_address",
    [DglabString_QrHint] = "qr_hint",
    [DglabString_QrNotRunning] = "qr_not_running",
    [DglabString_QrTooLong] = "qr_too_long",
    [DglabString_LogTitle] = "log_title",
    [DglabString_SleepWarning] = "sleep_warning",
    [DglabString_SleepWarningAutoOff] = "sleep_warning_auto_off",

    // what the buttons sent, and what came back
    [DglabString_CmdClear] = "cmd_clear",
    [DglabString_CmdTestA] = "cmd_test_a",
    [DglabString_CmdTestB] = "cmd_test_b",
    [DglabString_CmdUpA] = "cmd_up_a",
    [DglabString_CmdDownA] = "cmd_down_a",
    [DglabString_CmdUpB] = "cmd_up_b",
    [DglabString_CmdDownB] = "cmd_down_b",
    [DglabString_CmdWaveformA] = "cmd_waveform_a",
    [DglabString_CmdWaveformB] = "cmd_waveform_b",
    [DglabString_CmdOk] = "cmd_ok",
    [DglabString_CmdNoApp] = "cmd_no_app",
    [DglabString_CmdRejected] = "cmd_rejected",
    [DglabString_CmdSocketError] = "cmd_socket_error",
    [DglabString_CmdChannelZeroA] = "cmd_channel_zero_a",
    [DglabString_CmdChannelZeroB] = "cmd_channel_zero_b",
};

// One language as it was read: the file in a single block, and a pointer to the
// value of every key inside it. The pointers stay valid because the block is
// never written again.
typedef struct {
    char* text;
    size_t capacity;
    size_t used;
    const char* values[DglabString_Count];
} LanguageTable;

static LanguageTable g_tables[DglabLanguage_Count];
static DglabLanguage g_language = DglabLanguage_English;

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

const char* dglabStringKeyName(DglabString id)
{
    if (id < 0 || id >= DglabString_Count)
        return NULL;

    return kKeys[id];
}

static int keyIndexOf(const char* key)
{
    for (int i = 0; i < DglabString_Count; i++) {
        if (kKeys[i] != NULL && strcmp(kKeys[i], key) == 0)
            return i;
    }

    return -1;
}

// Only these two have a file; Auto is a preference, not a language.
static bool hasFile(DglabLanguage language)
{
    return language == DglabLanguage_ChineseSimplified || language == DglabLanguage_English;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

static void setProblem(DglabStringsReport* report, DglabStringsProblem problem, unsigned line)
{
    // The first problem is the one worth reporting: the rest are usually a
    // consequence of it.
    if (report->problem != DglabStringsProblem_None)
        return;

    report->problem = problem;
    report->line = line;
}

// For the mistakes that make the file useless whatever else is in it: naming
// those is more helpful than the first complaint about its contents.
static void forceProblem(DglabStringsReport* report, DglabStringsProblem problem, unsigned line)
{
    report->problem = problem;
    report->line = line;
}

static void setDetail(DglabStringsReport* report, const char* text)
{
    snprintf(report->detail, sizeof(report->detail), "%s", text != NULL ? text : "");
}

// Turns whatever the reader is unhappy about into the report. `what` describes
// the shape this loader expected, for the case where the JSON is valid but is
// not a language file.
static DglabStringsLoadResult failFromReader(DglabJsonReader* reader, DglabStringsReport* report,
    DglabJsonToken token, const char* what)
{
    if (dglabJsonError(reader) != NULL) {
        forceProblem(report, DglabStringsProblem_Syntax, dglabJsonErrorLine(reader));
        setDetail(report, dglabJsonError(reader));
    } else if (token == DglabJsonToken_EndOfInput) {
        // A file that stops in the middle of an object is not valid JSON, but
        // the reader only sees a file that ended, so the loader says what it
        // was waiting for.
        forceProblem(report, DglabStringsProblem_Syntax, dglabJsonTokenLine(reader));
        setDetail(report, "the file ends too early");
    } else {
        forceProblem(report, DglabStringsProblem_Structure, dglabJsonTokenLine(reader));
        setDetail(report, what);
    }

    return DglabStringsLoad_Failed;
}

// Copies one member name out of the reader, because the next token overwrites
// the text buffer it lives in.
static bool takeKey(DglabJsonReader* reader, char* key, size_t key_size,
    DglabStringsReport* report)
{
    if (dglabJsonTextLength(reader) >= key_size) {
        forceProblem(report, DglabStringsProblem_Structure, dglabJsonTokenLine(reader));
        setDetail(report, "member name too long");
        return false;
    }

    memcpy(key, dglabJsonText(reader), dglabJsonTextLength(reader));
    key[dglabJsonTextLength(reader)] = '\0';
    return true;
}

// The inside of "strings": key -> value, one entry per DglabString.
static DglabStringsLoadResult parseStrings(DglabJsonReader* reader, LanguageTable* table,
    DglabStringsReport* report)
{
    char key[DGLAB_STRING_KEY_MAX];
    bool seen[DglabString_Count];
    DglabJsonToken token;

    memset(seen, 0, sizeof(seen));

    token = dglabJsonNext(reader); // the opening brace was read by the caller

    while (token != DglabJsonToken_EndObject) {
        if (token != DglabJsonToken_String)
            return failFromReader(reader, report, token, "expected a key inside \"strings\"");

        if (!takeKey(reader, key, sizeof(key), report))
            return DglabStringsLoad_Failed;

        token = dglabJsonNext(reader);

        if (token != DglabJsonToken_Colon)
            return failFromReader(reader, report, token, "expected : after a key");

        token = dglabJsonNext(reader);

        if (token != DglabJsonToken_String)
            return failFromReader(reader, report, token,
                "every key inside \"strings\" needs a text value");

        if (key[0] != '_') {
            int index = keyIndexOf(key);

            if (index < 0) {
                report->unknown++;

                if (report->problem == DglabStringsProblem_None) {
                    setProblem(report, DglabStringsProblem_UnknownKeys, dglabJsonTokenLine(reader));
                    setDetail(report, key);
                }

                report->count++;
            } else if (seen[index]) {
                if (report->problem == DglabStringsProblem_None) {
                    setProblem(report, DglabStringsProblem_DuplicateKey, dglabJsonTokenLine(reader));
                    setDetail(report, key);
                    report->count = 1;
                }
            } else {
                size_t length = dglabJsonTextLength(reader);

                if (table->used + length + 1 > table->capacity) {
                    forceProblem(report, DglabStringsProblem_OutOfMemory, 0);
                    setDetail(report, "the text does not fit");
                    return DglabStringsLoad_Failed;
                }

                memcpy(table->text + table->used, dglabJsonText(reader), length);
                table->text[table->used + length] = '\0';
                table->values[index] = table->text + table->used;
                table->used += length + 1;
                seen[index] = true;
            }
        }

        token = dglabJsonNext(reader);

        if (token == DglabJsonToken_Comma) {
            token = dglabJsonNext(reader);

            // ",}" is not a member list: the file is edited by hand, so the
            // mistake is named instead of being skipped over.
            if (token == DglabJsonToken_EndObject)
                return failFromReader(reader, report, token, "trailing comma");
        } else if (token != DglabJsonToken_EndObject) {
            return failFromReader(reader, report, token, "expected , or } after a key");
        }
    }

    const char* first_missing = NULL;

    for (int i = 0; i < DglabString_Count; i++) {
        if (seen[i])
            continue;

        if (first_missing == NULL)
            first_missing = kKeys[i] != NULL ? kKeys[i] : "?";

        report->missing++;
    }

    // Missing keys are the last thing that can be known about a file, so they
    // never take the report away from an earlier problem.
    if (report->missing > 0 && report->problem == DglabStringsProblem_None) {
        setProblem(report, DglabStringsProblem_MissingKeys, 0);
        setDetail(report, first_missing);
        report->count = report->missing;
    }

    return report->problem == DglabStringsProblem_None ? DglabStringsLoad_Ok
                                                      : DglabStringsLoad_Incomplete;
}

// The whole file: an object with "language" (which has to name the language
// this file was loaded as) and "strings". Members this loader does not know are
// stepped over, so a file can carry notes and future fields.
static DglabStringsLoadResult parseLanguage(DglabLanguage language, const char* text, size_t size,
    LanguageTable* table, DglabStringsReport* report)
{
    DglabJsonReader reader;
    char buffer[DGLAB_STRING_VALUE_MAX];
    char key[DGLAB_STRING_KEY_MAX];
    bool have_language = false;
    bool have_strings = false;
    DglabJsonToken token;

    dglabJsonInit(&reader, text, size, buffer, sizeof(buffer));

    token = dglabJsonNext(&reader);

    if (token != DglabJsonToken_BeginObject)
        return failFromReader(&reader, report, token, "the file has to be one object");

    token = dglabJsonNext(&reader);

    while (token != DglabJsonToken_EndObject) {
        if (token != DglabJsonToken_String)
            return failFromReader(&reader, report, token, "expected a member name");

        if (!takeKey(&reader, key, sizeof(key), report))
            return DglabStringsLoad_Failed;

        token = dglabJsonNext(&reader);

        if (token != DglabJsonToken_Colon)
            return failFromReader(&reader, report, token, "expected : after a member name");

        token = dglabJsonNext(&reader);

        if (strcmp(key, "language") == 0) {
            if (token != DglabJsonToken_String)
                return failFromReader(&reader, report, token,
                    "\"language\" has to be a text value");

            snprintf(report->language, sizeof(report->language), "%s", dglabJsonText(&reader));
            have_language = true;

            if (strcmp(dglabJsonText(&reader), dglabLanguageKey(language)) != 0) {
                forceProblem(report, DglabStringsProblem_Language, dglabJsonTokenLine(&reader));
                setDetail(report, report->language);
                return DglabStringsLoad_Failed;
            }
        } else if (strcmp(key, "strings") == 0) {
            if (token != DglabJsonToken_BeginObject)
                return failFromReader(&reader, report, token, "\"strings\" has to be an object");

            // Two of them would mean one set of values is written over the
            // other, and the file has no room for both.
            if (have_strings) {
                forceProblem(report, DglabStringsProblem_Structure, dglabJsonTokenLine(&reader));
                setDetail(report, "two \"strings\" objects");
                return DglabStringsLoad_Failed;
            }

            have_strings = true;

            DglabStringsLoadResult result = parseStrings(&reader, table, report);

            if (result == DglabStringsLoad_Failed)
                return result;
        } else if (!dglabJsonSkipValue(&reader, token)) {
            // Skipping only fails at the end of the file or on a mistake inside
            // the value, and both of those are the reader's to explain.
            return failFromReader(&reader, report, DglabJsonToken_EndOfInput,
                "the value is not well formed");
        }

        token = dglabJsonNext(&reader);

        if (token == DglabJsonToken_Comma) {
            token = dglabJsonNext(&reader);

            if (token == DglabJsonToken_EndObject)
                return failFromReader(&reader, report, token, "trailing comma");
        } else if (token != DglabJsonToken_EndObject) {
            return failFromReader(&reader, report, token, "expected , or } after a member");
        }
    }

    if (!have_strings) {
        forceProblem(report, DglabStringsProblem_Structure, dglabJsonTokenLine(&reader));
        setDetail(report, "no \"strings\" object");
        return DglabStringsLoad_Failed;
    }

    if (!have_language) {
        forceProblem(report, DglabStringsProblem_Structure, dglabJsonTokenLine(&reader));
        setDetail(report, "no \"language\" member");
        return DglabStringsLoad_Failed;
    }

    return report->problem == DglabStringsProblem_None ? DglabStringsLoad_Ok
                                                      : DglabStringsLoad_Incomplete;
}

static void tableRelease(DglabLanguage language)
{
    if (!hasFile(language))
        return;

    free(g_tables[language].text);
    memset(&g_tables[language], 0, sizeof(g_tables[language]));
}

void dglabStringsReset(void)
{
    for (int i = 0; i < DglabLanguage_Count; i++)
        tableRelease((DglabLanguage)i);
}

DglabStringsLoadResult dglabStringsLoadJson(DglabLanguage language, const char* text, size_t size,
    DglabStringsReport* report)
{
    DglabStringsReport scratch;
    LanguageTable table;
    DglabStringsLoadResult result;

    if (report == NULL)
        report = &scratch;

    memset(report, 0, sizeof(*report));
    memset(&table, 0, sizeof(table));

    if (!hasFile(language)) {
        setProblem(report, DglabStringsProblem_Unsupported, 0);
        setDetail(report, dglabLanguageKey(language));
        return DglabStringsLoad_Failed;
    }

    if (text == NULL || size == 0) {
        setProblem(report, DglabStringsProblem_Structure, 0);
        setDetail(report, "the file is empty");
        return DglabStringsLoad_Failed;
    }

    // One block for the whole file: the values are copied into it as they are
    // read, and each of them ends with a terminator.
    table.capacity = size + DglabString_Count + 1;
    table.text = malloc(table.capacity);

    if (table.text == NULL) {
        setProblem(report, DglabStringsProblem_OutOfMemory, 0);
        setDetail(report, "no room for the file");
        return DglabStringsLoad_Failed;
    }

    table.text[0] = '\0';

    result = parseLanguage(language, text, size, &table, report);

    if (result == DglabStringsLoad_Failed) {
        free(table.text);
        return result;
    }

    // Only now is the old table dropped: a file that fails to load leaves the
    // language it replaces untouched.
    tableRelease(language);
    g_tables[language] = table;
    return result;
}

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------

void dglabStringsSetLanguage(DglabLanguage language)
{
    g_language = language == DglabLanguage_ChineseSimplified ? DglabLanguage_ChineseSimplified
                                                            : DglabLanguage_English;
}

const char* dglabStringFor(DglabLanguage language, DglabString id)
{
    if (id < 0 || id >= DglabString_Count)
        return "";

    if (hasFile(language) && g_tables[language].values[id] != NULL)
        return g_tables[language].values[id];

    // A language that has no file of its own follows the one that does, so a
    // single file on the card is enough to run the UI (docs/nro-ui.md).
    for (int i = 0; i < DglabLanguage_Count; i++) {
        if (!hasFile((DglabLanguage)i))
            continue;

        if (g_tables[i].values[id] != NULL)
            return g_tables[i].values[id];
    }

    return "";
}

const char* dglabString(DglabString id)
{
    return dglabStringFor(g_language, id);
}
