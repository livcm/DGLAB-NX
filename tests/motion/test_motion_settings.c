// Host side tests for the editable motion parameters.
//
// These are the values the advanced screen changes, so the guarantees that
// matter are: the defaults are exactly what the mode shipped with, one key press
// is worth exactly one step, the ranges clamp, related pairs stay consistent,
// and the config file round trips without a bad line taking anything else down.

#include <dglab/nro/motion_feed.h>
#include <dglab/nro/motion_settings.h>

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

#define CHECK(condition)                                                \
    do {                                                                \
        g_checks++;                                                     \
        if (!(condition)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            g_failures++;                                               \
        }                                                               \
    } while (0)

static void testDefaultsMatchTheMode(void)
{
    DglabMotionFeedConfig settings;
    DglabMotionFeedConfig shipped;

    dglabMotionFeedDefaultConfig(&shipped);
    dglabMotionSettingsDefault(&settings);

    CHECK(memcmp(&settings, &shipped, sizeof(settings)) == 0);
    CHECK(settings.frequency_fast_ms == 30);
    CHECK(settings.deadzone_enter > settings.deadzone_exit);

    // The density starts out variable, with the fixed interval sitting between
    // the two ends of the range it replaces.
    CHECK(!settings.density_fixed);
    CHECK(settings.frequency_fixed_ms == 65);
}

// The density switch is the one row that is a word rather than a number, and it
// obeys the same "one press, one step" rule as every other row.
static void testDensitySwitch(void)
{
    DglabMotionFeedConfig config;
    char text[32];

    dglabMotionSettingsDefault(&config);

    CHECK(!config.density_fixed);
    CHECK(dglabMotionSettingsIsSwitch(DglabMotionSetting_DensityFixed));
    CHECK(!dglabMotionSettingsIsSwitch(DglabMotionSetting_FrequencyFixed));
    CHECK(!dglabMotionSettingsIsSwitch(DglabMotionSetting_FrequencyStill));

    // What a console-free caller prints. The screen draws the localized words
    // instead, from the same field.
    dglabMotionSettingsFormat(&config, DglabMotionSetting_DensityFixed, text, sizeof(text));
    CHECK(strcmp(text, "variable") == 0);

    dglabMotionSettingsStep(&config, DglabMotionSetting_DensityFixed, 1);
    CHECK(config.density_fixed);

    dglabMotionSettingsFormat(&config, DglabMotionSetting_DensityFixed, text, sizeof(text));
    CHECK(strcmp(text, "fixed") == 0);

    // Clamped at both ends instead of wrapping.
    for (int i = 0; i < 5; i++)
        dglabMotionSettingsStep(&config, DglabMotionSetting_DensityFixed, 1);

    CHECK(config.density_fixed);

    for (int i = 0; i < 5; i++)
        dglabMotionSettingsStep(&config, DglabMotionSetting_DensityFixed, -1);

    CHECK(!config.density_fixed);

    // The interval it uses is an ordinary millisecond value with its own range.
    dglabMotionSettingsFormat(&config, DglabMotionSetting_FrequencyFixed, text, sizeof(text));
    CHECK(strcmp(text, "65ms") == 0);

    dglabMotionSettingsStep(&config, DglabMotionSetting_FrequencyFixed, 1);
    CHECK(config.frequency_fixed_ms == 70);

    for (int i = 0; i < 200; i++)
        dglabMotionSettingsStep(&config, DglabMotionSetting_FrequencyFixed, -1);

    CHECK(config.frequency_fixed_ms == 10);

    for (int i = 0; i < 200; i++)
        dglabMotionSettingsStep(&config, DglabMotionSetting_FrequencyFixed, 1);

    CHECK(config.frequency_fixed_ms == 500);
}

static void testOneStepIsOneStep(void)
{
    DglabMotionFeedConfig config;

    dglabMotionSettingsDefault(&config);

    // The frequency floor the mode is being tuned for: 30ms down towards the
    // device's 10ms, one press at a time.
    dglabMotionSettingsStep(&config, DglabMotionSetting_FrequencyFast, -1);
    CHECK(config.frequency_fast_ms == 25);
    dglabMotionSettingsStep(&config, DglabMotionSetting_FrequencyFast, -1);
    CHECK(config.frequency_fast_ms == 20);

    // Clamped at the range ends instead of wrapping or running away.
    for (int i = 0; i < 40; i++)
        dglabMotionSettingsStep(&config, DglabMotionSetting_FrequencyFast, -1);

    CHECK(config.frequency_fast_ms == 10);

    for (int i = 0; i < 40; i++)
        dglabMotionSettingsStep(&config, DglabMotionSetting_StrengthMax, 1);

    CHECK(config.strength_max == 100);

    for (int i = 0; i < 120; i++)
        dglabMotionSettingsStep(&config, DglabMotionSetting_StrengthMax, -1);

    CHECK(config.strength_max == 1);
}

static void testRelatedSettingsStayConsistent(void)
{
    DglabMotionFeedConfig config;

    dglabMotionSettingsDefault(&config);

    // The exit side of the dead zone never crosses the entry side.
    for (int i = 0; i < 100; i++)
        dglabMotionSettingsStep(&config, DglabMotionSetting_DeadzoneExit, 1);

    CHECK(config.deadzone_exit <= config.deadzone_enter);

    // And the fast frequency never ends up slower than the still one.
    for (int i = 0; i < 100; i++)
        dglabMotionSettingsStep(&config, DglabMotionSetting_FrequencyFast, 1);

    CHECK(config.frequency_fast_ms <= config.frequency_still_ms);
}

static void testFormatting(void)
{
    DglabMotionFeedConfig config;
    char text[32];

    dglabMotionSettingsDefault(&config);

    dglabMotionSettingsFormat(&config, DglabMotionSetting_FrequencyFast, text, sizeof(text));
    CHECK(strcmp(text, "30ms") == 0);

    dglabMotionSettingsFormat(&config, DglabMotionSetting_DeadzoneEnter, text, sizeof(text));
    CHECK(strcmp(text, "0.05") == 0);

    dglabMotionSettingsFormat(&config, DglabMotionSetting_GyroRange, text, sizeof(text));
    CHECK(strcmp(text, "6.00") == 0);

    // The strength ceiling is not a duration: it must not be labelled in ms.
    dglabMotionSettingsFormat(&config, DglabMotionSetting_StrengthMax, text, sizeof(text));
    CHECK(strcmp(text, "100") == 0);

    dglabMotionSettingsFormat(&config, DglabMotionSetting_Release, text, sizeof(text));
    CHECK(strcmp(text, "300ms") == 0);
}

static void testFileRoundTrip(void)
{
    DglabMotionFeedConfig written;
    DglabMotionFeedConfig read;
    char text[512];

    dglabMotionSettingsDefault(&written);
    dglabMotionSettingsStep(&written, DglabMotionSetting_FrequencyFast, -4); // 30 -> 10
    dglabMotionSettingsStep(&written, DglabMotionSetting_DeadzoneEnter, 3);  // 0.05 -> 0.08
    dglabMotionSettingsStep(&written, DglabMotionSetting_StrengthMax, -10);  // 100 -> 90
    dglabMotionSettingsStep(&written, DglabMotionSetting_DensityFixed, 1);   // off -> on
    dglabMotionSettingsStep(&written, DglabMotionSetting_FrequencyFixed, -5); // 65 -> 40

    dglabMotionSettingsSerialize(&written, text, sizeof(text));

    dglabMotionSettingsDefault(&read);
    dglabMotionSettingsParse(&read, text);

    CHECK(read.frequency_fast_ms == 10);
    CHECK(read.deadzone_enter == 0.08f);
    CHECK(read.strength_max == 90);
    CHECK(read.density_fixed);
    CHECK(read.frequency_fixed_ms == 40);
    CHECK(read.release_ms == written.release_ms);
}

// A file written before the density switch existed still loads: the keys it does
// not mention are left alone, which is what makes the switch a no-migration
// change for anyone with a tuned motion.cfg on their card.
static void testOlderFileLeavesTheNewKeysAlone(void)
{
    DglabMotionFeedConfig config;

    dglabMotionSettingsDefault(&config);
    // Values a file that predates the switch cannot carry.
    config.density_fixed = true;
    config.frequency_fixed_ms = 200;

    dglabMotionSettingsParse(&config,
        "deadzone_enter=0.05\n"
        "deadzone_exit=0.02\n"
        "gyro_range=6.00\n"
        "accel_range=2.00\n"
        "gyro_weight=1.00\n"
        "accel_weight=1.00\n"
        "attack_ms=50\n"
        "release_ms=300\n"
        "idle_stop_ms=250\n"
        "frequency_fast_ms=30\n"
        "frequency_still_ms=100\n"
        "strength_max=100\n");

    CHECK(config.density_fixed);
    CHECK(config.frequency_fixed_ms == 200);
    CHECK(config.frequency_still_ms == 100);
}

static void testBadLinesAreIgnored(void)
{
    DglabMotionFeedConfig config;

    dglabMotionSettingsDefault(&config);

    dglabMotionSettingsParse(&config,
        "frequency_fast_ms=15\n"
        "unknown_setting=7\n"
        "deadzone_enter=\n"
        "release_ms=not-a-number\n"
        "strength_max=999\n");

    CHECK(config.frequency_fast_ms == 15);
    CHECK(config.deadzone_enter == 0.05f);   // empty value: unchanged
    CHECK(config.release_ms == 300);         // garbage: unchanged
    CHECK(config.strength_max == 100);       // out of range: clamped, not rejected
}

int main(void)
{
    testDefaultsMatchTheMode();
    testDensitySwitch();
    testOneStepIsOneStep();
    testRelatedSettingsStayConsistent();
    testFormatting();
    testFileRoundTrip();
    testOlderFileLeavesTheNewKeysAlone();
    testBadLinesAreIgnored();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
