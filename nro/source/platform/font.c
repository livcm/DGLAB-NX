#include <dglab/platform/font.h>

#include <switch.h>

static bool g_pl_ready;
static bool g_system_is_chinese = true;
static DglabFontSet g_set;

bool dglabFontSystemIsChinese(void)
{
    u64 code = 0;
    SetLanguage language = SetLanguage_ENUS;

    if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &language)))
        return language == SetLanguage_ZHCN || language == SetLanguage_ZHTW;

    return g_system_is_chinese;
}

const DglabFontSet* dglabFontOpen(bool chinese)
{
    PlFontData font;
    Result rc;
    DglabTtfFont* faces[4];

    if (!g_pl_ready) {
        // pl:u, because the shared fonts are not available over pl:s on newer
        // system versions.
        rc = plInitialize(PlServiceType_User);

        if (R_FAILED(rc))
            return NULL;

        g_pl_ready = true;
    }

    g_system_is_chinese = dglabFontSystemIsChinese();

    rc = plGetSharedFontByType(&font,
        chinese ? PlSharedFontType_ChineseSimplified : PlSharedFontType_Standard);

    if (R_FAILED(rc) && chinese) {
        // A console that cannot provide the Chinese face still has the standard
        // one: better Chinese with missing glyphs than no text at all.
        rc = plGetSharedFontByType(&font, PlSharedFontType_Standard);
    }

    if (R_FAILED(rc))
        return NULL;

    // One face, four sizes: they share the font bytes but each keeps its own
    // glyph cache, so a page can draw its title, rows, values and notes at once.
    dglabTtfFontReset();

    faces[0] = dglabTtfFontCreate(font.address, font.size, DGLAB_TEXT_TITLE);
    faces[1] = dglabTtfFontCreate(font.address, font.size, DGLAB_TEXT_BODY);
    faces[2] = dglabTtfFontCreate(font.address, font.size, DGLAB_TEXT_VALUE);
    faces[3] = dglabTtfFontCreate(font.address, font.size, DGLAB_TEXT_NOTE);

    for (size_t i = 0; i < sizeof(faces) / sizeof(faces[0]); i++) {
        if (dglabTtfFontSource(faces[i]) == NULL) {
            dglabTtfFontReset();
            return NULL;
        }
    }

    g_set.title = dglabTtfFontSource(faces[0]);
    g_set.body = dglabTtfFontSource(faces[1]);
    g_set.value = dglabTtfFontSource(faces[2]);
    g_set.note = dglabTtfFontSource(faces[3]);

    return &g_set;
}

void dglabFontClose(void)
{
    dglabTtfFontReset();

    if (!g_pl_ready)
        return;

    plExit();
    g_pl_ready = false;
}
