#pragma once

// Editable motion mode parameters: the advanced screen changes these, the motion
// mode feeds them to dglabMotionFeedInit, and they are stored in one small text
// file so a tuning session survives a restart (see docs/joycon-input.md).
//
// Platform independent on purpose: the stepping, clamping, formatting and the
// file format are all covered by tests/motion.

#include <dglab/nro/motion_feed.h>

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    DglabMotionSetting_DeadzoneEnter = 0, ///< movement has to reach this to count
    DglabMotionSetting_DeadzoneExit,      ///< and fall below this to let go
    DglabMotionSetting_GyroRange,         ///< |angular velocity| that is full scale
    DglabMotionSetting_AccelRange,        ///< |acceleration change| that is full scale
    DglabMotionSetting_GyroWeight,
    DglabMotionSetting_AccelWeight,
    DglabMotionSetting_Attack,
    DglabMotionSetting_Release,
    DglabMotionSetting_IdleStop,
    DglabMotionSetting_FrequencyFast,  ///< interval at full intensity
    DglabMotionSetting_FrequencyStill, ///< interval while still
    DglabMotionSetting_DensityFixed,   ///< waveform density: variable or fixed
    DglabMotionSetting_FrequencyFixed, ///< the interval while it is fixed
    DglabMotionSetting_StrengthMax,    ///< waveform strength at full intensity
    DglabMotionSetting_Count,
} DglabMotionSetting;

/// Loads the values this project ships with. Same numbers as
/// dglabMotionFeedDefaultConfig(), which is what the motion mode used before the
/// parameters became editable.
void dglabMotionSettingsDefault(DglabMotionFeedConfig* config);

/// Moves one setting by `steps` of its own increment and clamps it. A single key
/// press has to be worth exactly one step, so callers pass +1 or -1 and never a
/// larger jump.
void dglabMotionSettingsStep(DglabMotionFeedConfig* config, unsigned setting, int steps);

/// The value as the screen shows it, e.g. "0.05", "6.00" or "30ms". The switch
/// row answers "fixed" or "variable" instead of a number; the screen draws the
/// localized words for it (dglab/ui/strings.h) and this is the plain word a
/// console-free caller prints.
void dglabMotionSettingsFormat(const DglabMotionFeedConfig* config, unsigned setting, char* out,
    size_t out_size);

/// True for the one setting that is a switch rather than a number. The screen
/// shows a word for it (the file still stores 0 or 1, and Format answers in the
/// same plain words the console-free caller can print).
bool dglabMotionSettingsIsSwitch(unsigned setting);

/// What the setting does, in one sentence.
const char* dglabMotionSettingDescription(unsigned setting);

/// Human readable name for the list.
const char* dglabMotionSettingName(unsigned setting);

/// Writes the settings as "key=value" lines. Anything the parser does not know
/// is ignored, so the format can grow without breaking old files.
void dglabMotionSettingsSerialize(const DglabMotionFeedConfig* config, char* out, size_t out_size);

/// Applies "key=value" lines onto `config`, clamping each value into range and
/// leaving everything else at whatever `config` already had.
void dglabMotionSettingsParse(DglabMotionFeedConfig* config, const char* text);
