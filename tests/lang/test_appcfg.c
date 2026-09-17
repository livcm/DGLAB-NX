// Host side tests for the NRO's own settings file, SD:/switch/DGLAB-NX/config/
// app.cfg - the `language=` and `theme=` lines dglab/ui/settings.h owns.
//
// It lives next to the language tests because it is the same kind of thing: a
// small text file on the SD card whose parser has to keep working across
// releases. The file the 0.3.0 build wrote holds one line, and that upgrade is
// one of the cases below.
//
// Run with: make -C tests/lang

#include <dglab/ui/settings.h>

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

static void testThemeModeHelpers(void)
{
    // The row's cycle: follow the console, light, dark, and back to following.
    CHECK(dglabThemeModeNext(DglabThemeMode_Auto) == DglabThemeMode_Light);
    CHECK(dglabThemeModeNext(DglabThemeMode_Light) == DglabThemeMode_Dark);
    CHECK(dglabThemeModeNext(DglabThemeMode_Dark) == DglabThemeMode_Auto);

    // The keys are what the file carries, so they have to round trip.
    CHECK(strcmp(dglabThemeModeKey(DglabThemeMode_Auto), "auto") == 0);
    CHECK(strcmp(dglabThemeModeKey(DglabThemeMode_Light), "light") == 0);
    CHECK(strcmp(dglabThemeModeKey(DglabThemeMode_Dark), "dark") == 0);

    for (int i = 0; i < DglabThemeMode_Count; i++) {
        DglabThemeMode mode = (DglabThemeMode)i;

        CHECK(dglabThemeModeFromKey(dglabThemeModeKey(mode)) == mode);
    }

    // Anything unknown is Auto, the same rule the language preference has: a
    // hand edited file must not be able to leave the UI in a state with no
    // palette at all.
    CHECK(dglabThemeModeFromKey("") == DglabThemeMode_Auto);
    CHECK(dglabThemeModeFromKey("Light") == DglabThemeMode_Auto);
    CHECK(dglabThemeModeFromKey("dark ") == DglabThemeMode_Auto);
    CHECK(dglabThemeModeFromKey("rainbow") == DglabThemeMode_Auto);
    CHECK(dglabThemeModeFromKey(NULL) == DglabThemeMode_Auto);

    // Which palette a preference means. Auto is the console's own theme, and the
    // fallback for a console that cannot answer is dark - the same thing an
    // unknown preference gets.
    CHECK(dglabThemeResolve(DglabThemeMode_Auto, true) == &dglabThemeDark);
    CHECK(dglabThemeResolve(DglabThemeMode_Auto, false) == &dglabThemeLight);
    CHECK(dglabThemeResolve(DglabThemeMode_Light, true) == &dglabThemeLight);
    CHECK(dglabThemeResolve(DglabThemeMode_Dark, false) == &dglabThemeDark);
    CHECK(dglabThemeResolve((DglabThemeMode)DglabThemeMode_Count, false) == &dglabThemeDark);

    // The two palettes are separate tables with the same fields: a value that
    // was only ever measured on one of them cannot leak into the other.
    CHECK(dglabThemeLight.background != dglabThemeDark.background);
    CHECK(dglabThemeLight.text != dglabThemeDark.text);
    CHECK(dglabThemeLight.rule != dglabThemeDark.rule);
    CHECK(dglabThemeLight.focus_fill != dglabThemeDark.focus_fill);
}

static void testDefaults(void)
{
    DglabAppSettings settings;

    // Nothing on the card: follow the console for both - which is the state a
    // fresh install starts in.
    dglabAppSettingsDefault(&settings);
    CHECK(settings.language == DglabLanguage_Auto);
    CHECK(settings.theme == DglabThemeMode_Auto);

    // A parser that reads nothing still leaves usable values behind.
    settings.language = DglabLanguage_English;
    settings.theme = DglabThemeMode_Dark;
    dglabAppSettingsParse(NULL, &settings);
    CHECK(settings.language == DglabLanguage_Auto);
    CHECK(settings.theme == DglabThemeMode_Auto);

    dglabAppSettingsParse("", &settings);
    CHECK(settings.language == DglabLanguage_Auto);
    CHECK(settings.theme == DglabThemeMode_Auto);
}

static void testRoundTrip(void)
{
    static const struct {
        DglabLanguage language;
        DglabThemeMode theme;
    } cases[] = {
        { DglabLanguage_Auto, DglabThemeMode_Auto },
        { DglabLanguage_ChineseSimplified, DglabThemeMode_Dark },
        { DglabLanguage_English, DglabThemeMode_Light },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        DglabAppSettings written;
        DglabAppSettings read;
        char text[128];

        written.language = cases[i].language;
        written.theme = cases[i].theme;

        dglabAppSettingsSerialize(&written, text, sizeof(text));
        dglabAppSettingsParse(text, &read);

        CHECK(read.language == written.language);
        CHECK(read.theme == written.theme);
    }

    // Both keys are in the file, so the second one cannot be a line the first
    // one's writer forgot to leave room for.
    {
        DglabAppSettings settings = { DglabLanguage_English, DglabThemeMode_Light };
        char text[128];

        dglabAppSettingsSerialize(&settings, text, sizeof(text));
        CHECK(strstr(text, "language=en") != NULL);
        CHECK(strstr(text, "theme=light") != NULL);
    }
}

// The upgrade path that matters: the build before this one wrote one line.
static void testOlderFileKeepsItsLanguage(void)
{
    DglabAppSettings settings;

    dglabAppSettingsParse("language=zh-Hans\n", &settings);
    CHECK(settings.language == DglabLanguage_ChineseSimplified);
    CHECK(settings.theme == DglabThemeMode_Auto);

    // The same file without the trailing newline, and with the CRLF a hand edit
    // on a PC can leave behind.
    dglabAppSettingsParse("language=zh-Hans", &settings);
    CHECK(settings.language == DglabLanguage_ChineseSimplified);

    dglabAppSettingsParse("language=en\r\n", &settings);
    CHECK(settings.language == DglabLanguage_English);
    CHECK(settings.theme == DglabThemeMode_Auto);
}

static void testUnknownLinesAreIgnored(void)
{
    DglabAppSettings settings;

    // The order in the file does not matter, and neither does a line this build
    // has never heard of: it is skipped rather than read as a setting.
    dglabAppSettingsParse("theme=light\nlanguage=en\nfuture=42\n", &settings);
    CHECK(settings.language == DglabLanguage_English);
    CHECK(settings.theme == DglabThemeMode_Light);

    // Values that are not one of the words mean Auto, exactly like a missing
    // line - the two ways a file goes wrong end in the same place.
    dglabAppSettingsParse("theme=rainbow\nlanguage=klingon\n", &settings);
    CHECK(settings.theme == DglabThemeMode_Auto);
    CHECK(settings.language == DglabLanguage_Auto);

    // A line with no "=" at all, an empty value and a key that is only a prefix
    // of a real one are all left alone.
    dglabAppSettingsParse("garbage\ntheme=\ntheme_extra=dark\n", &settings);
    CHECK(settings.theme == DglabThemeMode_Auto);

    // The last line that does name a setting wins, so an appended line is what
    // the file means.
    dglabAppSettingsParse("theme=light\ntheme=dark\n", &settings);
    CHECK(settings.theme == DglabThemeMode_Dark);
}

int main(void)
{
    testThemeModeHelpers();
    testDefaults();
    testRoundTrip();
    testOlderFileKeepsItsLanguage();
    testUnknownLinesAreIgnored();

    printf("%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
