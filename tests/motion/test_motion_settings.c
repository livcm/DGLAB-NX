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

    dglabMotionSettingsSerialize(&written, text, sizeof(text));

    dglabMotionSettingsDefault(&read);
    dglabMotionSettingsParse(&read, text);

    CHECK(read.frequency_fast_ms == 10);
    CHECK(read.deadzone_enter == 0.08f);
    CHECK(read.strength_max == 90);
    CHECK(read.release_ms == written.release_ms);
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
    testOneStepIsOneStep();
    testRelatedSettingsStayConsistent();
    testFormatting();
    testFileRoundTrip();
    testBadLinesAreIgnored();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
