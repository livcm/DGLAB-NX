#include <dglab/ui/screen.h>

#include <dglab/ui/list.h>
#include <dglab/ui/page.h>
#include <dglab/ui/qr.h>
#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>

#include <stdio.h>
#include <string.h>

// The socket page: the QR code the phone scans on the left with the counters
// under it, and the server's own parameters on the right. Everything the player
// can do here is a shortcut key, so nothing is focused and nothing scrolls -
// which is also why both columns have to fit the content column exactly
// (tests/canvas holds that).
//
// The sysmodule log is a page of its own (Y opens and closes it), drawn like the
// console's long text pages.

// The QR code: quiet zone in modules, the biggest module size worth drawing, and
// the margins the column keeps around it.
#define QR_QUIET_ZONE 4
#define QR_MAX_MODULE 8
#define QR_MIN_MODULE 4
#define QR_COLUMN_PAD 8
// The three counter lines under the QR, and how far above the bottom rule they
// stop.
#define COUNTER_LINES 3
#define COUNTER_LINE 24
#define COUNTERS_GAP 20
#define QR_BOTTOM_GAP 8
// Space between a hint line inside the content and the rows under it.
#define HINT_LINE_GAP 8

// Encoding costs a matrix build and the page is redrawn whenever one of its
// counters changes, so the result is kept until the payload itself changes.
static char g_cached_url[DGLAB_NET_QR_MAX];
static DglabQrCode g_cached_code;
static bool g_cached;

const char* dglabNetStateText(u32 state)
{
    switch (state) {
        case DglabNetState_Listening: return dglabString(DglabString_StateWaiting);
        case DglabNetState_Paired: return dglabString(DglabString_StateConnected);
        case DglabNetState_Stopped: return dglabString(DglabString_StateStopped);
        case DglabNetState_Failed: return dglabString(DglabString_StateFailed);
        default: return dglabString(DglabString_StateNotStarted);
    }
}

static uint32_t stateColor(u32 state)
{
    const DglabTheme* theme = dglabThemeGet();

    switch (state) {
        case DglabNetState_Paired: return theme->accent;
        case DglabNetState_Listening: return theme->warn;
        case DglabNetState_Failed: return theme->error;
        default: return theme->muted;
    }
}

static bool statusHasWarning(const DglabScreenState* state)
{
    return state->status_ok && (state->status.state == DglabNetState_Listening ||
        state->status.state == DglabNetState_Paired);
}

// The app id is a uuid. The two column layout is wide enough for the whole thing
// in the console's own font, and a uuid is only much use when it can be compared
// with the one the app shows - so it is shown in full, and only cut down to its
// ends when `room` says it does not fit (a wider font, or a longer id).
static void formatAppId(DglabGlyphSource* font, int room, const char* id, char* out,
    size_t out_size)
{
    if (id == NULL || id[0] == '\0') {
        snprintf(out, out_size, "-");
        return;
    }

    if (strlen(id) == 36 && dglabTextWidth(font, id) > room)
        snprintf(out, out_size, "%.8s...%.8s", id, id + 28);
    else
        snprintf(out, out_size, "%s", id);
}

// The code for the current payload, or NULL when there is nothing to show.
//
// A payload exists as soon as the console has a LAN address - that is what the
// sysmodule's NET_QR answers, and the address is worth showing on its own - but
// a code is only worth scanning while there is a socket listening behind it, so
// the screen is where the two are put together. Before the server runs the
// column says so instead (QrNotRunning), which is also what the reason below
// picks: a QR code nobody is listening on just wastes the user's scan.
static DglabQrCode* qrCode(const DglabScreenState* state)
{
    if (!state->url_ok || !state->url || state->url[0] == '\0')
        return NULL;

    if (!state->status_ok || !statusHasWarning(state))
        return NULL;

    if (!g_cached || strcmp(g_cached_url, state->url) != 0) {
        g_cached = dglabQrEncodeString(&g_cached_code, state->url, DglabQrEcc_M);
        snprintf(g_cached_url, sizeof(g_cached_url), "%s", state->url);
    }

    return g_cached ? &g_cached_code : NULL;
}

// One line of the counter block under the QR: the label in the muted colour and
// the value next to it, both in the small font.
static void drawCounter(DglabCanvas* canvas, const DglabFontSet* fonts, int x, int y,
    const char* label, const char* value)
{
    const DglabTheme* theme = dglabThemeGet();
    int label_width = dglabTextWidth(fonts->note, label);

    dglabTextDraw(canvas, fonts->note, x, y, label, theme->muted);
    dglabTextDraw(canvas, fonts->note, x + label_width + 12, y, value, theme->text);
}

// The left column: the hint line, the code, and the three counters. The counters
// sit at a fixed height so the column does not jump when the code appears or
// disappears.
static void drawQrColumn(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabScreenState* state)
{
    const DglabTheme* theme = dglabThemeGet();
    DglabQrCode* code = qrCode(state);
    char counters[64];
    char report[48];
    int counters_y = DGLAB_PAGE_CONTENT_BOTTOM - QR_BOTTOM_GAP - COUNTER_LINES * COUNTER_LINE;
    int qr_y = DGLAB_PAGE_CONTENT_TOP + DGLAB_NOTE_LINE + HINT_LINE_GAP;
    int room = counters_y - COUNTERS_GAP - qr_y;

    dglabTextDraw(canvas, fonts->note, DGLAB_SOCKET_QR_X, DGLAB_PAGE_CONTENT_TOP,
        dglabString(DglabString_QrHint), theme->muted);

    if (code == NULL) {
        // Why there is no code: the server is not running, or there is no LAN
        // address yet. The reason takes the place the code would have had.
        const char* reason;

        if (!state->status_ok)
            reason = dglabString(DglabString_StateIpcFailed);
        else if (state->status.state != DglabNetState_Listening &&
            state->status.state != DglabNetState_Paired)
            reason = dglabString(DglabString_QrNotRunning);
        else
            reason = dglabString(DglabString_NoAddress);

        {
            char line[192];
            const char* text = reason;
            int y = qr_y;

            while (*text) {
                size_t taken = dglabTextWrapLine(fonts->note, text,
                    DGLAB_SOCKET_QR_WIDTH - QR_COLUMN_PAD * 2, line, sizeof(line));

                if (taken == 0)
                    break;

                dglabTextDraw(canvas, fonts->note, DGLAB_SOCKET_QR_X + QR_COLUMN_PAD, y, line,
                    theme->warn);
                y += DGLAB_NOTE_LINE;
                text += taken;

                while (*text == ' ')
                    text++;
            }
        }
    } else {
        // The module size follows the column in both directions: the code has to
        // fit the width and the room the counters leave above the bottom rule.
        int total = code->size + QR_QUIET_ZONE * 2;
        int module = (DGLAB_SOCKET_QR_WIDTH - QR_COLUMN_PAD * 2) / total;
        int side;

        if (module > room / total)
            module = room / total;

        if (module > QR_MAX_MODULE)
            module = QR_MAX_MODULE;

        if (module < QR_MIN_MODULE) {
            dglabTextDraw(canvas, fonts->note, DGLAB_SOCKET_QR_X + QR_COLUMN_PAD, qr_y,
                dglabString(DglabString_QrTooLong), theme->error);
        } else {
            side = total * module;
            dglabCanvasQr(canvas, code, DGLAB_SOCKET_QR_X + (DGLAB_SOCKET_QR_WIDTH - side) / 2,
                qr_y, module, QR_QUIET_ZONE, theme->black, theme->white);
        }
    }

    snprintf(counters, sizeof(counters), "%u / %u / %u", (unsigned)state->status.sessions,
        (unsigned)state->status.commands_sent, (unsigned)state->status.reports_received);
    drawCounter(canvas, fonts, DGLAB_SOCKET_QR_X, counters_y,
        dglabString(DglabString_LabelCounters), counters);

    snprintf(counters, sizeof(counters), "%u / %u / %u", (unsigned)state->status.heartbeats_sent,
        (unsigned)state->status.messages_in, (unsigned)state->status.messages_out);
    drawCounter(canvas, fonts, DGLAB_SOCKET_QR_X, counters_y + COUNTER_LINE,
        dglabString(DglabString_LabelHeartbeats), counters);

    if (state->status.reports_received == 0)
        snprintf(report, sizeof(report), "%s", dglabString(DglabString_NoReport));
    else
        snprintf(report, sizeof(report), "%u/%u (%u/%u)", (unsigned)state->status.app_strength_a,
            (unsigned)state->status.app_strength_b, (unsigned)state->status.app_limit_a,
            (unsigned)state->status.app_limit_b);

    drawCounter(canvas, fonts, DGLAB_SOCKET_QR_X, counters_y + COUNTER_LINE * 2,
        dglabString(DglabString_LabelAppReport), report);
}

// The right column: the two adjustment hints, then the server's parameters. The
// rows are not focusable, so the list is drawn with focus = -1.
static void drawInfoColumn(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabScreenState* state)
{
    const DglabTheme* theme = dglabThemeGet();
    DglabListFonts list_fonts = { fonts->body, fonts->value, fonts->note };
    DglabHint adjust[2] = {
        { DglabButton_Up, DglabButton_Down, dglabString(DglabString_HintAdjustA) },
        { DglabButton_Left, DglabButton_Right, dglabString(DglabString_HintAdjustB) },
    };
    DglabRow rows[6];
    DglabRowBox boxes[6];
    DglabListStyle style;
    char address[96];
    char app_id[64];
    char strength_a[32];
    char strength_b[32];
    char command[64];
    int x = DGLAB_SOCKET_INFO_X;
    const DglabNetStatus* status = &state->status;

    for (int i = 0; i < 2; i++) {
        dglabHintDraw(canvas, fonts->icon, fonts->note, &adjust[i], x, DGLAB_PAGE_CONTENT_TOP,
            theme->text);
        x += dglabHintWidth(fonts->note, &adjust[i]) + DGLAB_PAGE_HINT_GAP;
    }

    if (status->ip_text[0])
        snprintf(address, sizeof(address), "%s:%u", (const char*)status->ip_text,
            (unsigned)status->port);
    else
        snprintf(address, sizeof(address), "%s", dglabString(DglabString_NoAddress));

    // What the id may occupy: the column, minus the row's insets, minus the
    // label it shares the row with.
    formatAppId(fonts->value,
        DGLAB_SOCKET_INFO_WIDTH - 2 * DGLAB_ROW_PAD -
            dglabTextWidth(fonts->body, dglabString(DglabString_LabelAppId)) - 24,
        (const char*)status->peer_id, app_id, sizeof(app_id));

    // value/ceiling: the number the D-pad dials, over the number it stops at
    // (dglabChannelCeiling()).
    snprintf(strength_a, sizeof(strength_a), "%u/%u", (unsigned)state->test_strength_a,
        (unsigned)state->limit_a);
    snprintf(strength_b, sizeof(strength_b), "%u/%u", (unsigned)state->test_strength_b,
        (unsigned)state->limit_b);
    snprintf(command, sizeof(command), "%s",
        (state->last_command && state->last_command[0]) ? state->last_command : "-");

    memset(rows, 0, sizeof(rows));

    rows[0] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_RowServer),
        .value = state->status_ok ? dglabNetStateText(status->state)
                                  : dglabString(DglabString_StateIpcFailed),
        .value_color = state->status_ok ? stateColor(status->state) : theme->error,
        // Two wordings for the same warning: once the front end has switched the
        // console's automatic sleep off (dglab/nro/auto_sleep.h), what is left to
        // watch out for is a sleep the user asks for.
        .note = statusHasWarning(state)
            ? dglabString(state->auto_sleep_suppressed ? DglabString_SleepWarningAutoOff
                                                       : DglabString_SleepWarning)
            : NULL,
    };
    rows[1] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_LabelAddress),
        .value = address,
        .value_color = status->ip_text[0] ? theme->text : theme->warn,
    };
    rows[2] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_LabelAppId),
        .value = app_id,
        .value_color = status->peer_id[0] ? theme->accent : theme->muted,
    };
    rows[3] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_LabelChannelA),
        .value = strength_a,
        .value_color = theme->text,
    };
    rows[4] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_LabelChannelB),
        .value = strength_b,
        .value_color = theme->text,
    };
    rows[5] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_CommandLabel),
        .value = command,
        .value_color = state->last_command_tone == DglabCmdTone_Error ? theme->error
            : (state->last_command_tone == DglabCmdTone_Warn ? theme->warn : theme->text),
    };

    dglabListMeasure(&list_fonts, rows, 6, DGLAB_SOCKET_INFO_WIDTH, boxes, 6);

    style = (DglabListStyle){
        .x = DGLAB_SOCKET_INFO_X,
        .origin_y = DGLAB_PAGE_CONTENT_TOP + DGLAB_NOTE_LINE + HINT_LINE_GAP,
        .width = DGLAB_SOCKET_INFO_WIDTH,
        .focus = -1,
        .navigation = false,
    };
    dglabListDraw(canvas, &list_fonts, &style, rows, boxes, 6);
}

static void drawSocketPage(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabScreenState* state)
{
    const DglabTheme* theme = dglabThemeGet();
    DglabTextStyle title = { fonts->title, theme->text };
    DglabHint hints[5];

    dglabPageBegin(canvas);
    dglabPageHeader(canvas, &title, dglabString(DglabString_SocketTitle));
    dglabPageHeaderStatus(canvas, fonts->value, state->sysmodule_ok);
    dglabPageClipWide(canvas);

    drawQrColumn(canvas, fonts, state);
    drawInfoColumn(canvas, fonts, state);

    dglabCanvasClearClip(canvas);

    hints[0] = (DglabHint){ DglabButton_ZL, DglabButton_ZR,
        dglabString(DglabString_ActionTestChannels), };
    hints[1] = (DglabHint){ DglabButton_X, DglabButton_None,
        dglabString(DglabString_ActionClear), };
    hints[2] = (DglabHint){ DglabButton_Y, DglabButton_None,
        dglabString(DglabString_ActionLog), };
    hints[3] = (DglabHint){ DglabButton_B, DglabButton_None,
        dglabString(DglabString_ActionBack), };
    hints[4] = (DglabHint){ DglabButton_A, DglabButton_None,
        statusHasWarning(state) ? dglabString(DglabString_ActionStop)
                                : dglabString(DglabString_ActionStart), };
    dglabPageHints(canvas, fonts->icon, fonts->body, hints, 5);
}

int dglabLogPageMaxOffset(int count)
{
    int view_height = DGLAB_PAGE_CONTENT_BOTTOM - DGLAB_PAGE_CONTENT_TOP;
    int content_height = count * DGLAB_SCREEN_LOG_PITCH;

    return content_height > view_height ? content_height - view_height : 0;
}

// The log page: the console's long text look - white body text at a 37px pitch -
// scrolled with up and down. The newest line is at the bottom when it opens.
// Both the socket page (the sysmodule log) and the Bluetooth page (the session
// log) show one, so it takes its content and its title and knows nothing about
// either.
void dglabLogPageDraw(DglabCanvas* canvas, const DglabFontSet* fonts, const DglabLogPage* log)
{
    const DglabTheme* theme = dglabThemeGet();
    DglabTextStyle title = { fonts->title, theme->text };
    DglabListPage page;
    DglabHint hints[2];
    int view_height = DGLAB_PAGE_CONTENT_BOTTOM - DGLAB_PAGE_CONTENT_TOP;
    int content_height = log->count * DGLAB_SCREEN_LOG_PITCH;

    // The page is a list of lines rather than of rows, but it scrolls like one:
    // the same layout decides how far the offset may go and whether the bar is
    // drawn at all.
    page = dglabListPageLayout(DGLAB_PAGE_CONTENT_TOP, view_height, content_height,
        log->offset);

    dglabPageBegin(canvas);
    dglabPageHeader(canvas, &title, log->title);
    dglabPageHeaderStatus(canvas, fonts->value, log->sysmodule_ok);
    dglabPageClipContent(canvas);

    for (int i = 0; i < log->count; i++) {
        int y = DGLAB_PAGE_CONTENT_TOP - page.offset + i * DGLAB_SCREEN_LOG_PITCH;

        dglabTextDraw(canvas, fonts->body, DGLAB_PAGE_CONTENT_X, y, log->lines[i],
            theme->text);
    }

    dglabCanvasClearClip(canvas);

    dglabListPageScrollBar(canvas, &page);

    // No scroll hint: the bar on the right edge says the log is longer than the
    // screen, and up/down still move it one line per press (docs/nro-ui.md).
    hints[0] = (DglabHint){ DglabButton_Y, DglabButton_None,
        dglabString(DglabString_ActionClose), };
    hints[1] = (DglabHint){ DglabButton_B, DglabButton_None,
        dglabString(DglabString_ActionBack), };
    dglabPageHints(canvas, fonts->icon, fonts->body, hints, 2);
}

static void drawLogPage(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabScreenState* state)
{
    DglabLogPage log;

    log.title = dglabString(DglabString_LogTitle);
    log.lines = state->log_lines;
    log.count = state->log_count;
    log.offset = state->log_offset;
    log.sysmodule_ok = state->sysmodule_ok;

    dglabLogPageDraw(canvas, fonts, &log);
}

void dglabScreenDraw(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabScreenState* state)
{
    if (state->log_open)
        drawLogPage(canvas, fonts, state);
    else
        drawSocketPage(canvas, fonts, state);
}
