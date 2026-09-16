#include <dglab/platform/font.h>

#include <dglab/platform/framebuffer.h>

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
    DglabTtfFont* faces[5];
    // The glyphs are rasterised at the resolution the framebuffer is running at;
    // every metric the layout reads stays in logical pixels (docs/nro-ui.md).
    static const float sizes[5] = {
        DGLAB_TEXT_TITLE, DGLAB_TEXT_BODY, DGLAB_TEXT_VALUE, DGLAB_TEXT_NOTE,
        DGLAB_TEXT_ICON,
    };
    int scale_num = 1;
    int scale_den = 1;

    dglabFramebufferScale(&scale_num, &scale_den);

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

    // One face, one object per size: they share the font bytes but each keeps its
    // own glyph cache, so a page can draw its title, rows, values, notes and
    // button letters at once.
    dglabTtfFontReset();

    for (size_t i = 0; i < sizeof(faces) / sizeof(faces[0]); i++)
        faces[i] = dglabTtfFontCreate(font.address, font.size, sizes[i], scale_num, scale_den);

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
    g_set.icon = dglabTtfFontSource(faces[4]);

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
