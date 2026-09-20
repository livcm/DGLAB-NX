#include <dglab/nro/motion_settings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One row of the advanced screen: range, increment, and how to read and write
// the field it points at.
typedef struct {
    const char* key;   // name in the config file
    const char* label; // name on screen
    float min;
    float max;
    float step;
    int decimals; // 0 for whole milliseconds, 2 for the scaled values
} SettingRange;

static const SettingRange kRanges[DglabMotionSetting_Count] = {
    { "deadzone_enter", "deadzone enter", 0.01f, 0.50f, 0.01f, 2 },
    { "deadzone_exit", "deadzone exit", 0.01f, 0.50f, 0.01f, 2 },
    { "gyro_range", "gyro range", 1.0f, 30.0f, 0.5f, 2 },
    { "accel_range", "accel range", 0.25f, 10.0f, 0.25f, 2 },
    { "gyro_weight", "gyro weight", 0.0f, 3.0f, 0.1f, 2 },
    { "accel_weight", "accel weight", 0.0f, 3.0f, 0.1f, 2 },
    { "attack_ms", "attack", 10.0f, 1000.0f, 10.0f, 0 },
    { "release_ms", "release", 25.0f, 2000.0f, 25.0f, 0 },
    { "idle_stop_ms", "idle stop", 100.0f, 2000.0f, 50.0f, 0 },
    { "frequency_fast_ms", "freq fast", 10.0f, 500.0f, 5.0f, 0 },
    { "frequency_still_ms", "freq still", 10.0f, 1000.0f, 10.0f, 0 },
    // The density switch is stored as 0/1 like every other line, but the screen
    // shows the two words below instead of the number.
    { "density_fixed", "density", 0.0f, 1.0f, 1.0f, 0 },
    { "frequency_fixed_ms", "fixed density", 10.0f, 500.0f, 5.0f, 0 },
    { "strength_max", "strength max", 1.0f, 100.0f, 1.0f, 0 },
};

// Only these are milliseconds. The strength ceiling is a plain 0..100 number, and
// the scaled values have no unit at all - the settings screen showing "100ms" for
// the waveform strength was simply wrong.
static bool settingIsMilliseconds(unsigned setting)
{
    return setting == DglabMotionSetting_Attack || setting == DglabMotionSetting_Release ||
           setting == DglabMotionSetting_IdleStop || setting == DglabMotionSetting_FrequencyFast ||
           setting == DglabMotionSetting_FrequencyStill ||
           setting == DglabMotionSetting_FrequencyFixed;
}

// The one setting that is a switch: its value is a word on screen, not a number.
bool dglabMotionSettingsIsSwitch(unsigned setting)
{
    return setting == DglabMotionSetting_DensityFixed;
}

static bool* boolField(DglabMotionFeedConfig* config, unsigned setting)
{
    switch (setting) {
        case DglabMotionSetting_DensityFixed: return &config->density_fixed;
        default: return NULL;
    }
}

static float* floatField(DglabMotionFeedConfig* config, unsigned setting)
{
    switch (setting) {
        case DglabMotionSetting_DeadzoneEnter: return &config->deadzone_enter;
        case DglabMotionSetting_DeadzoneExit: return &config->deadzone_exit;
        case DglabMotionSetting_GyroRange: return &config->gyro_reference;
        case DglabMotionSetting_AccelRange: return &config->accel_reference;
        case DglabMotionSetting_GyroWeight: return &config->gyro_weight;
        case DglabMotionSetting_AccelWeight: return &config->accel_weight;
        default: return NULL;
    }
}

// The millisecond fields are stored as integers; they are edited through the
// same float ranges so one code path does stepping and clamping.
static uint32_t* msField(DglabMotionFeedConfig* config, unsigned setting)
{
    switch (setting) {
        case DglabMotionSetting_Attack: return &config->attack_ms;
        case DglabMotionSetting_Release: return &config->release_ms;
        case DglabMotionSetting_IdleStop: return &config->idle_stop_ms;
        default: return NULL;
    }
}

static float settingValue(const DglabMotionFeedConfig* config, unsigned setting)
{
    DglabMotionFeedConfig* mutable_config = (DglabMotionFeedConfig*)config;
    bool* b = boolField(mutable_config, setting);
    float* f = floatField(mutable_config, setting);
    uint32_t* ms = msField(mutable_config, setting);

    if (b)
        return *b ? 1.0f : 0.0f;

    if (f)
        return *f;

    if (ms)
        return (float)*ms;

    switch (setting) {
        case DglabMotionSetting_FrequencyFast: return (float)config->frequency_fast_ms;
        case DglabMotionSetting_FrequencyStill: return (float)config->frequency_still_ms;
        case DglabMotionSetting_FrequencyFixed: return (float)config->frequency_fixed_ms;
        case DglabMotionSetting_StrengthMax: return (float)config->strength_max;
        default: return 0.0f;
    }
}

static float clampf(float value, float min, float max)
{
    if (value < min)
        return min;

    if (value > max)
        return max;

    return value;
}

// Keeps the pairs sane after any change: the release side of the dead zone below
// its entry side, and the fast frequency no slower than the still one.
static void fixRelations(DglabMotionFeedConfig* config)
{
    if (config->deadzone_exit > config->deadzone_enter)
        config->deadzone_exit = config->deadzone_enter;

    if (config->frequency_fast_ms > config->frequency_still_ms)
        config->frequency_fast_ms = config->frequency_still_ms;
}

static void setSetting(DglabMotionFeedConfig* config, unsigned setting, float value)
{
    const SettingRange* range = &kRanges[setting];
    bool* b = boolField(config, setting);
    float* f = floatField(config, setting);
    uint32_t* ms = msField(config, setting);

    value = clampf(value, range->min, range->max);

    if (b) {
        *b = value >= 0.5f;
    } else if (f) {
        *f = value;
    } else if (ms) {
        *ms = (uint32_t)(value + 0.5f);
    } else {
        switch (setting) {
            case DglabMotionSetting_FrequencyFast:
                config->frequency_fast_ms = (uint16_t)(value + 0.5f);
                break;
            case DglabMotionSetting_FrequencyStill:
                config->frequency_still_ms = (uint16_t)(value + 0.5f);
                break;
            case DglabMotionSetting_FrequencyFixed:
                config->frequency_fixed_ms = (uint16_t)(value + 0.5f);
                break;
            case DglabMotionSetting_StrengthMax:
                config->strength_max = (uint8_t)(value + 0.5f);
                break;
            default:
                break;
        }
    }

    fixRelations(config);
}

void dglabMotionSettingsDefault(DglabMotionFeedConfig* config)
{
    if (!config)
        return;

    dglabMotionFeedDefaultConfig(config);
}

void dglabMotionSettingsStep(DglabMotionFeedConfig* config, unsigned setting, int steps)
{
    float value;

    if (!config || setting >= DglabMotionSetting_Count)
        return;

    value = settingValue(config, setting) + (float)steps * kRanges[setting].step;

    setSetting(config, setting, value);
}

void dglabMotionSettingsFormat(const DglabMotionFeedConfig* config, unsigned setting, char* out,
    size_t out_size)
{
    const SettingRange* range;
    char number[32];

    if (!config || !out || out_size == 0 || setting >= DglabMotionSetting_Count)
        return;

    range = &kRanges[setting];

    // The switch has no unit and no number: the screen replaces this with the
    // localized word, and this is what a console-free caller prints.
    if (dglabMotionSettingsIsSwitch(setting)) {
        snprintf(out, out_size, "%s",
            settingValue(config, setting) >= 0.5f ? "fixed" : "variable");
        return;
    }

    if (range->decimals == 0)
        snprintf(number, sizeof(number), "%.0f", (double)settingValue(config, setting));
    else
        snprintf(number, sizeof(number), "%.2f", (double)settingValue(config, setting));

    if (range->decimals == 0 && settingIsMilliseconds(setting))
        snprintf(out, out_size, "%sms", number);
    else
        snprintf(out, out_size, "%s", number);
}

const char* dglabMotionSettingName(unsigned setting)
{
    if (setting >= DglabMotionSetting_Count)
        return "?";

    return kRanges[setting].label;
}

const char* dglabMotionSettingDescription(unsigned setting)
{
    switch (setting) {
        case DglabMotionSetting_DeadzoneEnter:
            return "How much movement is needed before anything is output. Raise it if a hand "
                   "that merely holds a Joy-Con still produces output.";
        case DglabMotionSetting_DeadzoneExit:
            return "How far the movement has to fall before the mode counts as still again. It "
                   "sits below the enter value so the level does not flicker at the edge.";
        case DglabMotionSetting_GyroRange:
            return "The rotation speed that counts as full output. Smaller means a gentler "
                   "movement already reaches the top.";
        case DglabMotionSetting_AccelRange:
            return "The acceleration change that counts as full output: this is the term that "
                   "reacts to a sudden jerk rather than to steady movement.";
        case DglabMotionSetting_GyroWeight:
        case DglabMotionSetting_AccelWeight:
            return "How much this term contributes. Set one to 0 to find out what the other one "
                   "is responsible for.";
        case DglabMotionSetting_Attack:
            return "How quickly the output follows a movement. Too short and every reading shows "
                   "up as a spike.";
        case DglabMotionSetting_Release:
            return "How long the output takes to fall back to silence after the movement stops.";
        case DglabMotionSetting_IdleStop:
            return "How long the controller has to stay still before uploading stops completely. "
                   "The release still finishes first.";
        case DglabMotionSetting_FrequencyFast:
            return "The pulse interval at full intensity. Smaller is denser: more pulses per "
                   "second at the same amplitude is also more current, so this is a strength "
                   "change too. The device floor is 10ms.";
        case DglabMotionSetting_FrequencyStill:
            return "The pulse interval while still. This is what the output starts from as a "
                   "movement fades out.";
        case DglabMotionSetting_DensityFixed:
            return "Variable lets the pulse interval follow the mode - the motion mode follows "
                   "the swing, the touch mode follows how far right the finger is. Fixed holds "
                   "the interval at the value below, so only the waveform value moves.";
        case DglabMotionSetting_FrequencyFixed:
            return "The pulse interval used while the density is fixed. Smaller is denser: more "
                   "pulses per second at the same amplitude is also more current. The device "
                   "floor is 10ms.";
        case DglabMotionSetting_StrengthMax:
            return "The waveform strength at full intensity, on top of the channel strength set "
                   "in Socket test. The device multiplies the two.";
        default:
            return "";
    }
}

void dglabMotionSettingsSerialize(const DglabMotionFeedConfig* config, char* out, size_t out_size)
{
    size_t written = 0;

    if (!config || !out || out_size == 0)
        return;

    out[0] = '\0';

    for (unsigned setting = 0; setting < (unsigned)DglabMotionSetting_Count; setting++) {
        char value[32];
        int result;

        if (kRanges[setting].decimals == 0)
            snprintf(value, sizeof(value), "%.0f", (double)settingValue(config, setting));
        else
            snprintf(value, sizeof(value), "%.2f", (double)settingValue(config, setting));

        result = snprintf(out + written, out_size - written, "%s=%s\n", kRanges[setting].key, value);

        if (result <= 0 || (size_t)result >= out_size - written)
            return;

        written += (size_t)result;
    }
}

void dglabMotionSettingsParse(DglabMotionFeedConfig* config, const char* text)
{
    const char* cursor = text;

    if (!config || !text)
        return;

    while (*cursor) {
        const char* line_end = strchr(cursor, '\n');
        size_t line_length = line_end ? (size_t)(line_end - cursor) : strlen(cursor);
        char line[64];
        char* equals;

        if (line_length >= sizeof(line))
            line_length = sizeof(line) - 1;

        memcpy(line, cursor, line_length);
        line[line_length] = '\0';

        equals = strchr(line, '=');

        if (equals) {
            char* value_text;

            *equals = '\0';
            value_text = equals + 1;

            if (*value_text) {
                char* end = NULL;
                double value = strtod(value_text, &end);

                // A malformed value leaves the setting as it was rather than
                // silently resetting it to something else.
                if (end && end != value_text) {
                    for (unsigned setting = 0; setting < (unsigned)DglabMotionSetting_Count;
                         setting++) {
                        if (strcmp(line, kRanges[setting].key) == 0) {
                            setSetting(config, setting, (float)value);
                            break;
                        }
                    }
                }
            }
        }

        if (!line_end)
            break;

        cursor = line_end + 1;
    }

    fixRelations(config);
}
