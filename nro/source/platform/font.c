#include <dglab/platform/font.h>

#include <switch.h>

static bool g_pl_ready;
static bool g_system_is_chinese = true;

bool dglabFontSystemIsChinese(void)
{
    u64 code = 0;
    SetLanguage language = SetLanguage_ENUS;

    if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &language)))
        return language == SetLanguage_ZHCN || language == SetLanguage_ZHTW;

    return g_system_is_chinese;
}

DglabGlyphSource* dglabFontOpen(bool chinese, float pixel_height)
{
    PlFontData font;
    Result rc;

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

    if (!dglabTtfFontInit(font.address, font.size, pixel_height))
        return NULL;

    return dglabTtfFontSource();
}

void dglabFontClose(void)
{
    if (!g_pl_ready)
        return;

    plExit();
    g_pl_ready = false;
}
