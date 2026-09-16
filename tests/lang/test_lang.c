// Host side tests for the language files: the JSON reader, the loader behind
// dglab/ui/strings.h, and the two files the NRO ships.
//
// Run with: make -C tests/lang

#include <dglab/util/json.h>
#include <dglab/platform/langfiles.h>
#include <dglab/ui/strings.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "lang_fixture.h"

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

// A file with two of the keys, which is enough to see what a complete file
// cannot show: what a half filled one does.
static const char* const kSmall =
    "{\n"
    "  \"language\": \"en\",\n"
    "  \"strings\": {\n"
    "    \"menu_title\": \"DGLAB-NX\",\n"
    "    \"sysmodule_ok\": \"ok\"\n"
    "  }\n"
    "}\n";

// The members of a "strings" object, one per key the UI has. Every value names
// the key it belongs to, so a test can tell which entry it got back.
// The value buildMembers() writes for one key: the file is built by walking the
// keys in enum order, so a test that moves a key cannot read "text 0" any more.
static const char* expectedValue(DglabString id)
{
    static char value[32];

    snprintf(value, sizeof(value), "text %u", (unsigned)id);

    return value;
}

static void buildMembers(char* out, size_t out_size, const char* prefix)
{
    size_t used = 0;
    bool first = true;

    for (int i = 0; i < DglabString_Count; i++) {
        const char* key = dglabStringKeyName((DglabString)i);

        if (key == NULL)
            continue;

        used += (size_t)snprintf(out + used, out_size - used, "%s\"%s\": \"%s%u\"",
            first ? "" : ", ", key, prefix, (unsigned)i);
        first = false;
    }
}

// A file that carries every key, complete enough for the loader to accept.
static void buildFile(char* out, size_t out_size, const char* language, const char* prefix)
{
    char members[16 * 1024];

    buildMembers(members, sizeof(members), prefix);
    snprintf(out, out_size, "{\"language\": \"%s\", \"strings\": {%s}}", language, members);
}

static void testKeyNames(void)
{
    for (int i = 0; i < DglabString_Count; i++) {
        const char* key = dglabStringKeyName((DglabString)i);

        CHECK(key != NULL);

        if (key == NULL)
            continue;

        CHECK(key[0] != '\0');
        CHECK(strchr(key, ' ') == NULL);
        CHECK(strchr(key, '"') == NULL);

        // Two entries with the same key would leave one of them unreachable in
        // every language file.
        for (int other = 0; other < i; other++) {
            const char* earlier = dglabStringKeyName((DglabString)other);

            if (earlier != NULL && strcmp(earlier, key) == 0)
                CHECK(false);
        }
    }

    CHECK(dglabStringKeyName(DglabString_Count) == NULL);
}

static void testReader(void)
{
    DglabJsonReader reader;
    char buffer[128];
    const char* text = "{ \"a\": [1, true, null], \"b\": \"x\" }";

    dglabJsonInit(&reader, text, strlen(text), buffer, sizeof(buffer));

    CHECK(dglabJsonNext(&reader) == DglabJsonToken_BeginObject);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_String);
    CHECK(strcmp(dglabJsonText(&reader), "a") == 0);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_Colon);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_BeginArray);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_Number);
    CHECK(strcmp(dglabJsonText(&reader), "1") == 0);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_Comma);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_True);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_Comma);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_Null);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_EndArray);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_Comma);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_String);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_Colon);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_String);
    CHECK(strcmp(dglabJsonText(&reader), "x") == 0);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_EndObject);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_EndOfInput);
    CHECK(dglabJsonError(&reader) == NULL);
}

static void testReaderLinesAndEscapes(void)
{
    DglabJsonReader reader;
    char buffer[128];
    // Line 3 holds the interesting one: an escaped quote, a newline, and a CJK
    // character once by escape and once as a surrogate pair.
    const char* text =
        "{\n"
        "  \"a\": \"x\",\n"
        "  \"b\": \"\\\"q\\\"\\n\\u4e2d \\ud83d\\ude00\"\n"
        "}\n";
    const char* expected = "\"q\"\n\xe4\xb8\xad \xf0\x9f\x98\x80";

    dglabJsonInit(&reader, text, strlen(text), buffer, sizeof(buffer));

    CHECK(dglabJsonNext(&reader) == DglabJsonToken_BeginObject);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_String);
    CHECK(dglabJsonTokenLine(&reader) == 2);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_Colon);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_String);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_Comma);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_String);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_Colon);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_String);
    CHECK(dglabJsonTokenLine(&reader) == 3);
    CHECK(strcmp(dglabJsonText(&reader), expected) == 0);
    CHECK(dglabJsonTextLength(&reader) == strlen(expected));
}

static void checkSyntaxError(const char* text, unsigned line)
{
    DglabJsonReader reader;
    char buffer[128];
    DglabJsonToken token;

    dglabJsonInit(&reader, text, strlen(text), buffer, sizeof(buffer));

    do {
        token = dglabJsonNext(&reader);
    } while (token != DglabJsonToken_None && token != DglabJsonToken_EndOfInput);

    CHECK(dglabJsonError(&reader) != NULL);
    CHECK(dglabJsonErrorLine(&reader) == line);
}

static void testReaderErrors(void)
{
    checkSyntaxError("{ \"a\": \"unterminated }", 1);
    checkSyntaxError("{ \"a\": tru }", 1);
    checkSyntaxError("{ \"a\": @ }", 1);
    checkSyntaxError("{ \"a\": \"\\q\" }", 1);
    checkSyntaxError("{ \"a\": \"\\u12\" }", 1);
    checkSyntaxError("{ \"a\": \"\\ud83d\" }", 1);
    checkSyntaxError("{ \"a\": \"\\ude00\" }", 1);
    checkSyntaxError("{ \"a\": \"x", 1);
}

static void testReaderSkipsValues(void)
{
    DglabJsonReader reader;
    char buffer[128];
    const char* text = "{\"a\": {\"b\": [1, {\"c\": \"d\"}]}, \"e\": 2}";
    DglabJsonToken token;

    dglabJsonInit(&reader, text, strlen(text), buffer, sizeof(buffer));

    CHECK(dglabJsonNext(&reader) == DglabJsonToken_BeginObject);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_String);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_Colon);
    token = dglabJsonNext(&reader);
    CHECK(token == DglabJsonToken_BeginObject);
    CHECK(dglabJsonSkipValue(&reader, token));
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_Comma);
    CHECK(dglabJsonNext(&reader) == DglabJsonToken_String);
    CHECK(strcmp(dglabJsonText(&reader), "e") == 0);
}

static void testIncompleteFileStillLoads(void)
{
    DglabStringsReport report;
    DglabStringsLoadResult result;

    dglabStringsReset();
    result = dglabStringsLoadJson(DglabLanguage_English, kSmall, strlen(kSmall), &report);

    CHECK(result == DglabStringsLoad_Incomplete);
    CHECK(report.problem == DglabStringsProblem_MissingKeys);
    CHECK(report.missing == (unsigned)DglabString_Count - 2u);
    CHECK(report.unknown == 0u);
    CHECK(strcmp(report.language, "en") == 0);
    CHECK(report.detail[0] != '\0');

    // What the file carried is there; what it left out is empty.
    CHECK(strcmp(dglabStringFor(DglabLanguage_English, DglabString_MenuTitle), "DGLAB-NX") == 0);
    CHECK(strcmp(dglabStringFor(DglabLanguage_English, DglabString_ActionBack), "") == 0);
}

static void testShippedFiles(void)
{
    char message[512] = { 0 };
    char path[512];
    char raw[16 * 1024];
    size_t size = 0;
    FILE* file;
    DglabStringsReport report;

    // Loading both the way the NRO does: a file that lost a key fails here.
    CHECK(dglabTestLoadLanguages(message, sizeof(message)));
    printf("  %s\n", message[0] == '\0' ? "en.json and zh-Hans.json loaded cleanly" : message);

    for (int i = 0; i < DglabString_Count; i++) {
        const char* english = dglabStringFor(DglabLanguage_English, (DglabString)i);
        const char* chinese = dglabStringFor(DglabLanguage_ChineseSimplified, (DglabString)i);

        CHECK(english[0] != '\0');
        CHECK(chinese[0] != '\0');
    }

    // And once more straight from the file, so the reader is exercised on the
    // text the repository actually ships.
    snprintf(path, sizeof(path), "%s/en.json", DGLAB_TEST_LANG_DIR);
    file = fopen(path, "rb");
    CHECK(file != NULL);

    if (file != NULL) {
        size = fread(raw, 1, sizeof(raw), file);
        fclose(file);

        dglabStringsReset();
        CHECK(dglabStringsLoadJson(DglabLanguage_English, raw, size, &report) ==
            DglabStringsLoad_Ok);
        CHECK(report.problem == DglabStringsProblem_None);
        CHECK(report.missing == 0u);
        CHECK(report.unknown == 0u);
        CHECK(strcmp(dglabStringFor(DglabLanguage_English, DglabString_MenuTitle),
                  "DGLAB-NX") == 0);
    }
}

static void testRejectsWhatIsNotAFile(void)
{
    DglabStringsReport report;
    const char* text;

    dglabStringsReset();

    CHECK(dglabStringsLoadJson(DglabLanguage_English, "", 0, &report) ==
        DglabStringsLoad_Failed);
    CHECK(report.problem == DglabStringsProblem_Structure);

    text = "[1, 2]";
    CHECK(dglabStringsLoadJson(DglabLanguage_English, text, strlen(text), &report) ==
        DglabStringsLoad_Failed);
    CHECK(report.problem == DglabStringsProblem_Structure);

    text = "{\"language\": \"en\"}";
    CHECK(dglabStringsLoadJson(DglabLanguage_English, text, strlen(text), &report) ==
        DglabStringsLoad_Failed);
    CHECK(report.problem == DglabStringsProblem_Structure);
    CHECK(strstr(report.detail, "strings") != NULL);

    text = "{\"strings\": {}}";
    CHECK(dglabStringsLoadJson(DglabLanguage_English, text, strlen(text), &report) ==
        DglabStringsLoad_Failed);
    CHECK(report.problem == DglabStringsProblem_Structure);
    CHECK(strstr(report.detail, "language") != NULL);

    text = "{\"language\": \"zh-Hans\", \"strings\": {}}";
    CHECK(dglabStringsLoadJson(DglabLanguage_English, text, strlen(text), &report) ==
        DglabStringsLoad_Failed);
    CHECK(report.problem == DglabStringsProblem_Language);
    CHECK(strcmp(report.language, "zh-Hans") == 0);

    text = "{\"language\": \"en\", \"strings\": {\"menu_title\": 3}}";
    CHECK(dglabStringsLoadJson(DglabLanguage_English, text, strlen(text), &report) ==
        DglabStringsLoad_Failed);
    CHECK(report.problem == DglabStringsProblem_Structure);

    text = "{\"language\": \"en\", \"strings\": {\"menu_title\": \"x\"}";
    CHECK(dglabStringsLoadJson(DglabLanguage_English, text, strlen(text), &report) ==
        DglabStringsLoad_Failed);
    CHECK(report.problem == DglabStringsProblem_Syntax);
    CHECK(report.line > 0u);

    text = "{\"language\": \"en\", \"strings\": {\"menu_title\": \"x\",}}";
    CHECK(dglabStringsLoadJson(DglabLanguage_English, text, strlen(text), &report) ==
        DglabStringsLoad_Failed);
    CHECK(report.problem == DglabStringsProblem_Structure);
    CHECK(strstr(report.detail, "trailing comma") != NULL);

    text = "{\"language\": \"en\", \"strings\": {}, \"strings\": {}}";
    CHECK(dglabStringsLoadJson(DglabLanguage_English, text, strlen(text), &report) ==
        DglabStringsLoad_Failed);
    CHECK(report.problem == DglabStringsProblem_Structure);
    CHECK(strstr(report.detail, "strings") != NULL);

    // Auto is a preference, not a language with a file of its own.
    CHECK(dglabStringsLoadJson(DglabLanguage_Auto, kSmall, strlen(kSmall), &report) ==
        DglabStringsLoad_Failed);
    CHECK(report.problem == DglabStringsProblem_Unsupported);
}

static void testWarnsAboutUnknownAndDuplicateKeys(void)
{
    DglabStringsReport report;
    const char* text;

    dglabStringsReset();

    text = "{\"language\": \"en\", \"strings\": {\"menu_titel\": \"typo\"}}";
    CHECK(dglabStringsLoadJson(DglabLanguage_English, text, strlen(text), &report) ==
        DglabStringsLoad_Incomplete);
    CHECK(report.problem == DglabStringsProblem_UnknownKeys);
    CHECK(report.unknown == 1u);
    CHECK(strcmp(report.detail, "menu_titel") == 0);
    CHECK(strcmp(dglabStringFor(DglabLanguage_English, DglabString_MenuTitle), "") == 0);

    dglabStringsReset();

    text = "{\"language\": \"en\", \"strings\": {\"menu_title\": \"a\", \"menu_title\": \"b\"}}";
    CHECK(dglabStringsLoadJson(DglabLanguage_English, text, strlen(text), &report) ==
        DglabStringsLoad_Incomplete);
    CHECK(report.problem == DglabStringsProblem_DuplicateKey);
    // The first of the two wins, so a file that repeats a key stays predictable.
    CHECK(strcmp(dglabStringFor(DglabLanguage_English, DglabString_MenuTitle), "a") == 0);
}

static void testMetadataIsIgnored(void)
{
    char members[16 * 1024];
    char text[17 * 1024];
    DglabStringsReport report;

    dglabStringsReset();
    buildMembers(members, sizeof(members), "text ");

    // The shipped files carry a "_readme" note and have to be able to grow a
    // field the loader does not know, so both sit in front of what it reads.
    snprintf(text, sizeof(text),
        "{\"_readme\": \"ignored\", \"version\": 2, \"extra\": {\"deep\": [1, 2]}, "
        "\"language\": \"en\", \"strings\": {\"_note\": \"also ignored\", %s}}",
        members);

    CHECK(dglabStringsLoadJson(DglabLanguage_English, text, strlen(text), &report) ==
        DglabStringsLoad_Ok);
    CHECK(report.problem == DglabStringsProblem_None);
    CHECK(report.unknown == 0u);
    CHECK(strcmp(dglabStringFor(DglabLanguage_English, DglabString_MenuTitle),
        expectedValue(DglabString_MenuTitle)) == 0);
}

static void testFailedLoadKeepsTheOldLanguage(void)
{
    char text[16 * 1024];
    DglabStringsReport report;

    dglabStringsReset();
    buildFile(text, sizeof(text), "en", "text ");

    CHECK(dglabStringsLoadJson(DglabLanguage_English, text, strlen(text), &report) ==
        DglabStringsLoad_Ok);
    CHECK(strcmp(dglabStringFor(DglabLanguage_English, DglabString_MenuTitle),
        expectedValue(DglabString_MenuTitle)) == 0);

    CHECK(dglabStringsLoadJson(DglabLanguage_English, "{ nope", 6, &report) ==
        DglabStringsLoad_Failed);
    CHECK(strcmp(dglabStringFor(DglabLanguage_English, DglabString_MenuTitle),
        expectedValue(DglabString_MenuTitle)) == 0);
}

static void testOneFileIsEnoughToRun(void)
{
    char text[16 * 1024];
    DglabStringsReport report;

    // Only the Chinese file is on the card: the English lookups follow it
    // instead of coming back empty.
    dglabStringsReset();
    buildFile(text, sizeof(text), "zh-Hans", "text ");

    CHECK(dglabStringsLoadJson(DglabLanguage_ChineseSimplified, text, strlen(text), &report) ==
        DglabStringsLoad_Ok);
    CHECK(strcmp(dglabStringFor(DglabLanguage_English, DglabString_MenuTitle),
        expectedValue(DglabString_MenuTitle)) == 0);

    dglabStringsSetLanguage(DglabLanguage_English);
    CHECK(strcmp(dglabString(DglabString_MenuTitle), expectedValue(DglabString_MenuTitle)) == 0);
}

// Where the tests build a small SD card. Relative on purpose: the Makefile runs
// the binary from tests/lang.
#define TEST_ROOT "build/langtest"

static void makeDir(const char* path)
{
    mkdir(path, 0777);
}

// Copies one of the shipped files into `directory`, the directory the loader is
// told to read the languages from.
static bool copyLanguageFile(const char* directory, const char* code)
{
    char source[512];
    char target[512];
    char text[16 * 1024];
    size_t size;
    FILE* file;
    FILE* out;

    snprintf(source, sizeof(source), "%s/%s.json", DGLAB_TEST_LANG_DIR, code);
    file = fopen(source, "rb");

    if (file == NULL)
        return false;

    size = fread(text, 1, sizeof(text), file);
    fclose(file);

    snprintf(target, sizeof(target), "%s/%s.json", directory, code);
    out = fopen(target, "wb");

    if (out == NULL)
        return false;

    fwrite(text, 1, size, out);
    fclose(out);
    return true;
}

// The reading itself. langfiles.c is the layer that talks to the SD card, and
// the only thing it needs from the platform is fopen, so a directory on the
// host drives the same code that runs on the console. In the NRO that directory
// is DGLAB_LANG_DIR and nothing else (the check below pins it).
static void testFindsFilesOnDisk(void)
{
    DglabLangReport report;

    CHECK(strcmp(DGLAB_LANG_DIR, "sdmc:/switch/DGLAB-NX/lang") == 0);

    makeDir(TEST_ROOT);
    makeDir(TEST_ROOT "/two");

    CHECK(copyLanguageFile(TEST_ROOT "/two", "en"));
    CHECK(copyLanguageFile(TEST_ROOT "/two", "zh-Hans"));

    dglabStringsReset();
    dglabLangFilesLoad(TEST_ROOT "/two", &report);

    CHECK(report.loaded);
    CHECK(report.complete);
    CHECK(report.languages == 2u);
    CHECK(report.notes == 0u);
    CHECK(report.error[0] == '\0');
    CHECK(strcmp(dglabStringFor(DglabLanguage_English, DglabString_MenuTitle),
              "DGLAB-NX") == 0);
    CHECK(strcmp(dglabStringFor(DglabLanguage_ChineseSimplified, DglabString_MenuTitle),
              "DGLAB-NX") == 0);
}

// One file is enough: what is missing is a note in the log, not a reason to
// refuse to start.
static void testOneFileOnDiskIsEnough(void)
{
    DglabLangReport report;

    makeDir(TEST_ROOT);
    makeDir(TEST_ROOT "/one");

    CHECK(copyLanguageFile(TEST_ROOT "/one", "en"));

    dglabStringsReset();
    dglabLangFilesLoad(TEST_ROOT "/one", &report);

    CHECK(report.loaded);
    CHECK(!report.complete);
    CHECK(report.languages == 1u);
    CHECK(report.notes >= 1u);
    CHECK(strstr(report.note[0], "zh-Hans") != NULL);

    // The lookups of the language that has no file follow the one that loaded.
    CHECK(strcmp(dglabStringFor(DglabLanguage_English, DglabString_MenuTitle),
              "DGLAB-NX") == 0);
    CHECK(strcmp(dglabStringFor(DglabLanguage_ChineseSimplified, DglabString_MenuTitle),
              "DGLAB-NX") == 0);
}

// Nothing to read: the report is what the console shows instead of starting.
static void testReportsWhenNothingIsOnTheCard(void)
{
    DglabLangReport report;

    makeDir(TEST_ROOT);
    makeDir(TEST_ROOT "/empty");

    dglabStringsReset();
    dglabLangFilesLoad(TEST_ROOT "/empty", &report);

    CHECK(!report.loaded);
    CHECK(!report.complete);
    CHECK(report.languages == 0u);
    CHECK(report.error[0] != '\0');
    CHECK(strstr(report.error, "en.json") != NULL);
    CHECK(strstr(report.error, "zh-Hans.json") != NULL);
    // It says where it looked, which is the only thing that lets the user fix it.
    CHECK(strstr(report.error, TEST_ROOT "/empty") != NULL);
    CHECK(report.notes >= 1u);
}

static void testBrokenFileIsReportedAndSkipped(void)
{
    DglabLangReport report;
    FILE* file;

    makeDir(TEST_ROOT);
    makeDir(TEST_ROOT "/broken");

    file = fopen(TEST_ROOT "/broken/en.json", "wb");
    CHECK(file != NULL);

    if (file != NULL) {
        fputs("{\"language\": \"en\", \"strings\": {", file);
        fclose(file);
    }

    CHECK(copyLanguageFile(TEST_ROOT "/broken", "zh-Hans"));

    dglabStringsReset();
    dglabLangFilesLoad(TEST_ROOT "/broken", &report);

    // The broken English file does not take the working Chinese one down with
    // it, and the reason is named.
    CHECK(report.loaded);
    CHECK(!report.complete);
    CHECK(report.languages == 1u);
    CHECK(report.notes >= 1u);
    CHECK(strstr(report.note[0], "en.json") != NULL);
    CHECK(strcmp(dglabStringFor(DglabLanguage_English, DglabString_MenuTitle),
              "DGLAB-NX") == 0);
}

int main(void)
{
    testKeyNames();
    testReader();
    testReaderLinesAndEscapes();
    testReaderErrors();
    testReaderSkipsValues();
    testIncompleteFileStillLoads();
    testShippedFiles();
    testRejectsWhatIsNotAFile();
    testWarnsAboutUnknownAndDuplicateKeys();
    testMetadataIsIgnored();
    testFailedLoadKeepsTheOldLanguage();
    testOneFileIsEnoughToRun();
    testFindsFilesOnDisk();
    testOneFileOnDiskIsEnough();
    testReportsWhenNothingIsOnTheCard();
    testBrokenFileIsReportedAndSkipped();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
