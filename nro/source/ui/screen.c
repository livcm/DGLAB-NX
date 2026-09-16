#include <dglab/ui/screen.h>

#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>
#include <dglab/ui/qr.h>

#include <stdio.h>
#include <string.h>

// Colours come from the active theme (nro/include/dglab/ui/theme.h); the names
// below are only there to keep the drawing code readable.
#define kBackground (dglabThemeGet()->background)
#define kPanel (dglabThemeGet()->panel)
#define kPanelBorder (dglabThemeGet()->panel_border)
#define kSelected (dglabThemeGet()->selected)
#define kText (dglabThemeGet()->text)
#define kMuted (dglabThemeGet()->muted)
#define kAccent (dglabThemeGet()->accent)
#define kWarn (dglabThemeGet()->warn)
#define kError (dglabThemeGet()->error)
#define kWhite (dglabThemeGet()->white)
#define kBlack (dglabThemeGet()->black)

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720
#define MARGIN 24
#define GAP 16
#define TITLE_HEIGHT 56
#define STATUS_WIDTH 560
// Sized so the panels hold their content: the status panel's ten rows plus the
// one-line sleep warning, and the log panel's DGLAB_SCREEN_LOG_LINES rows.
#define STATUS_HEIGHT 272
#define LOG_HEIGHT 280
#define LINE_HEIGHT 20
#define LOG_CHARS ((STATUS_WIDTH - 32) / 16)
#define QR_QUIET_ZONE 4
#define QR_MAX_MODULE 10


static void drawPanel(DglabCanvas* canvas, DglabGlyphSource* text, int x, int y, int width,
    int height, const char* title)
{
    dglabCanvasFill(canvas, x, y, width, height, kPanel);
    dglabCanvasFrame(canvas, x, y, width, height, 2, kPanelBorder);

    if (title)
        dglabTextDraw(canvas, text, x + 12, y + 8, title, kMuted);
}

// A URL has no words to break at: it is cut exactly where the width ends.
static void drawHardWrapped(DglabCanvas* canvas, DglabGlyphSource* text, int x, int y, int width,
    const char* value, uint32_t color)
{
    char line[192];

    while (*value) {
        size_t take = 0;
        size_t offset = 0;

        while (value[offset]) {
            size_t used = 0;
            size_t next = offset;

            // One character at a time, so the cut lands on a character
            // boundary even for multi byte text.
            do {
                next++;
            } while (((unsigned char)value[next] & 0xC0) == 0x80);

            if (dglabTextWidth(text, line) > width && offset > 0)
                break;

            (void)used;
            memcpy(line, value, next);
            line[next] = '\0';
            offset = next;
        }

        if (offset == 0)
            break;

        take = offset;

        if (take > sizeof(line) - 1)
            break;

        dglabTextDraw(canvas, text, x, y, line, color);

        y += text->line_height;
        value += take;
    }
}

// Wrapping comes from the text layer: it measures with real glyph widths and
// knows that Chinese breaks between characters while "192.168.1.161" does not.
static void drawWrapped(DglabCanvas* canvas, DglabGlyphSource* text, int x, int y, int width,
    const char* value, uint32_t color)
{
    char line[192];

    while (*value) {
        size_t taken = dglabTextWrapLine(text, value, width, line, sizeof(line));

        if (taken == 0)
            break;

        dglabTextDraw(canvas, text, x, y, line, color);

        y += text->line_height;
        value += taken;

        while (*value == ' ')
            value++;
    }
}

// Uuids do not fit the panel, so only their ends are shown.
static void formatShortId(const char* id, char* out, size_t out_size)
{
    if (strlen(id) == 36)
        snprintf(out, out_size, "%.8s...%.8s", id, id + 28);
    else
        snprintf(out, out_size, "%s", id);
}

static const char* stateText(u32 state)
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
    switch (state) {
        case DglabNetState_Paired: return kAccent;
        case DglabNetState_Listening: return kWarn;
        case DglabNetState_Failed: return kError;
        default: return kMuted;
    }
}

static void drawTitle(DglabCanvas* canvas, DglabGlyphSource* text,
    const DglabScreenState* state)
{
    char port[64];
    char title[128];
    char right[64];

    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, TITLE_HEIGHT, kPanel);

    snprintf(port, sizeof(port), dglabString(DglabString_SocketPort), (unsigned)state->status.port);
    snprintf(title, sizeof(title), "%s   %s", dglabString(DglabString_SocketTitle), port);
    dglabTextDraw(canvas, text, MARGIN, (TITLE_HEIGHT - text->cell_height) / 2, title, kText);

    snprintf(right, sizeof(right), "IPC %u.%u.%u", state->version.major, state->version.minor,
        state->version.patch);
    dglabTextDraw(canvas, text, SCREEN_WIDTH - MARGIN - dglabTextWidth(text, right),
        (TITLE_HEIGHT - text->cell_height) / 2, right, kMuted);
}

// Label column: a floor, not a rule. The label is measured as well, because the
// console's font is wider than the one the layout was drawn against - "channel
// strength" used to run straight into its value.
#define LABEL_COLUMN 200

static void drawStatusLine(DglabCanvas* canvas, DglabGlyphSource* text, int x, int y, const char* label,
    const char* value, uint32_t color)
{
    int measured = dglabTextWidth(text, label) + 24;

    dglabTextDraw(canvas, text, x, y, label, kMuted);
    dglabTextDraw(canvas, text, x + (measured > LABEL_COLUMN ? measured : LABEL_COLUMN), y, value,
        color);
}

static void drawStatus(DglabCanvas* canvas, DglabGlyphSource* text,
    const DglabScreenState* state)
{
    const DglabNetStatus* status = &state->status;
    int line = text->line_height;
    int x = MARGIN + 16;
    int y = TITLE_HEIGHT + GAP + 24 + line;
    // Header, eight rows and the one line sleep warning.
    int panel_height = 24 + line * 10;
    char buffer[160];

    drawPanel(canvas, text, MARGIN, TITLE_HEIGHT + GAP, STATUS_WIDTH, panel_height,
        dglabString(DglabString_PanelServer));

    if (!state->status_ok) {
        drawStatusLine(canvas, text, x, y, dglabString(DglabString_LabelState),
            dglabString(DglabString_LinkIpcFailed), kError);
        return;
    }

    drawStatusLine(canvas, text, x, y, dglabString(DglabString_LabelState), stateText(status->state),
        stateColor(status->state));
    y += line;

    if (status->ip_text[0]) {
        snprintf(buffer, sizeof(buffer), "%s:%u", (const char*)status->ip_text,
            (unsigned)status->port);
    } else {
        snprintf(buffer, sizeof(buffer), "%s", dglabString(DglabString_NoAddress));
    }

    drawStatusLine(canvas, text, x, y, dglabString(DglabString_LabelAddress), buffer,
        status->ip_text[0] ? kText : kWarn);
    y += line;

    if (status->peer_id[0])
        formatShortId((const char*)status->peer_id, buffer, sizeof(buffer));
    else
        snprintf(buffer, sizeof(buffer), "-");

    drawStatusLine(canvas, text, x, y, dglabString(DglabString_LabelAppId), buffer,
        status->peer_id[0] ? kAccent : kMuted);
    y += line;

    snprintf(buffer, sizeof(buffer), "%u / %u / %u", (unsigned)status->sessions,
        (unsigned)status->commands_sent, (unsigned)status->reports_received);
    drawStatusLine(canvas, text, x, y, dglabString(DglabString_LabelCounters), buffer, kText);
    y += line;

    snprintf(buffer, sizeof(buffer), "%u / %u / %u", (unsigned)status->heartbeats_sent,
        (unsigned)status->messages_in, (unsigned)status->messages_out);
    drawStatusLine(canvas, text, x, y, dglabString(DglabString_LabelHeartbeats), buffer, kText);
    y += line;

    if (status->reports_received == 0)
        snprintf(buffer, sizeof(buffer), "%s", dglabString(DglabString_NoReport));
    else
        snprintf(buffer, sizeof(buffer), "%u/%u (%u/%u)", (unsigned)status->app_strength_a,
            (unsigned)status->app_strength_b, (unsigned)status->app_limit_a,
            (unsigned)status->app_limit_b);

    drawStatusLine(canvas, text, x, y, dglabString(DglabString_LabelAppReport), buffer, kMuted);
    y += line;

    drawStatusLine(canvas, text, x, y, dglabString(DglabString_CommandLabel), 
        (state->last_command && state->last_command[0]) ? state->last_command : "-",
        state->last_command_tone == DglabCmdTone_Error ? kError :
            (state->last_command_tone == DglabCmdTone_Warn ? kWarn : kText));
    y += line;

    snprintf(buffer, sizeof(buffer), "A %u/100   B %u/100", (unsigned)state->test_strength_a,
        (unsigned)state->test_strength_b);
    drawStatusLine(canvas, text, x, y, dglabString(DglabString_LabelStrength), buffer, kMuted);
    y += line;

    if (status->state == DglabNetState_Listening || status->state == DglabNetState_Paired) {
        y += line / 2;
        drawWrapped(canvas, text, x, y, STATUS_WIDTH - 32,
            dglabString(DglabString_SleepWarning), kWarn);
    }
}

static void drawQr(DglabCanvas* canvas, DglabGlyphSource* text, const DglabScreenState* state)
{
    // Encoding costs a handful of matrix builds, so the result is cached until
    // the payload changes.
    static char cached_url[DGLAB_NET_QR_MAX];
    static DglabQrCode cached_code;
    static bool cached = false;
    int panel_x = MARGIN * 2 + STATUS_WIDTH;
    int panel_y = TITLE_HEIGHT + GAP;
    int panel_w = SCREEN_WIDTH - panel_x - MARGIN;
    int panel_h = SCREEN_HEIGHT - panel_y - text->line_height * 2 - 32;
    int total;
    int module;
    int qr_size;

    drawPanel(canvas, text, panel_x, panel_y, panel_w, panel_h, dglabString(DglabString_QrHint));

    if (state->status.state != DglabNetState_Listening &&
        state->status.state != DglabNetState_Paired) {
        drawWrapped(canvas, text, panel_x + 20, panel_y + 60, panel_w - 40,
            dglabString(DglabString_QrNotRunning), kWarn);
        return;
    }

    if (!state->url_ok || !state->url || state->url[0] == '\0') {
        drawWrapped(canvas, text, panel_x + 20, panel_y + 60, panel_w - 40,
            dglabString(DglabString_QrNoAddress), kWarn);
        return;
    }

    if (!cached || strcmp(cached_url, state->url) != 0) {
        if (!dglabQrEncodeString(&cached_code, state->url, DglabQrEcc_M)) {
            dglabTextDraw(canvas, text, panel_x + 20, panel_y + 60,
                dglabString(DglabString_QrTooLong), kError);
            return;
        }

        snprintf(cached_url, sizeof(cached_url), "%s", state->url);
        cached = true;
    }

    // The code takes the space above the wrapped url text, and the module size
    // follows from that.
    module = (panel_h - 160) / (cached_code.size + QR_QUIET_ZONE * 2);

    if (module > QR_MAX_MODULE)
        module = QR_MAX_MODULE;

    if (module < 2)
        module = 2;

    qr_size = (cached_code.size + QR_QUIET_ZONE * 2) * module;
    total = (panel_w - qr_size) / 2;

    dglabCanvasQr(canvas, &cached_code, panel_x + total, panel_y + 40, module, QR_QUIET_ZONE,
        kBlack, kWhite);

    drawHardWrapped(canvas, text, panel_x + 20, panel_y + 40 + qr_size + 16, panel_w - 40,
        state->url, kMuted);
}

// The log keeps libnx's 16px bitmap font: twelve dense lines fit at that size,
// and the sysmodule writes English, so nothing here needs the CJK font. The
// panel shows the newest DGLAB_SCREEN_LOG_VISIBLE lines of the ring.
static void drawLog(DglabCanvas* canvas, DglabGlyphSource* text, const DglabFont* font,
    const DglabScreenState* state, int y, int height)
{
    int x = MARGIN + 16;
    char line[DGLAB_SCREEN_LOG_LINE_LEN + 1];
    int fits = (height - 40) / 20;
    int shown = state->log_count < fits ? state->log_count : fits;
    int first = state->log_count - shown;
    int text_y = y + 40;

    if (shown < 0)
        shown = 0;

    dglabCanvasFill(canvas, MARGIN, y, STATUS_WIDTH, height, kPanel);
    dglabCanvasFrame(canvas, MARGIN, y, STATUS_WIDTH, height, 2, kPanelBorder);
    dglabTextDraw(canvas, text, MARGIN + 12, y + 6, dglabString(DglabString_LogTitle), kMuted);

    for (int i = 0; i < shown; i++) {
        const char* source = state->log_lines[first + i];
        size_t length = strlen(source);

        if (length > DGLAB_SCREEN_LOG_LINE_LEN)
            length = DGLAB_SCREEN_LOG_LINE_LEN;

        memcpy(line, source, length);
        line[length] = '\0';

        dglabCanvasText(canvas, font, x, text_y + i * 20, 1, line, kMuted);
    }
}

static void drawFooter(DglabCanvas* canvas, DglabGlyphSource* text)
{
    dglabTextDraw(canvas, text, MARGIN, SCREEN_HEIGHT - text->line_height * 2 - 16,
        dglabString(DglabString_SocketKeys), kText);
    dglabTextDraw(canvas, text, MARGIN, SCREEN_HEIGHT - text->line_height - 16,
        dglabString(DglabString_SocketValues), kText);
}

void dglabScreenDraw(DglabCanvas* canvas, DglabGlyphSource* text, const DglabFont* log_font,
    const DglabScreenState* state)
{
    int log_y;
    int log_height;

    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, kBackground);

    drawTitle(canvas, text, state);
    drawStatus(canvas, text, state);
    drawQr(canvas, text, state);

    // The log takes what is left of the left column, above the footer.
    log_y = TITLE_HEIGHT + GAP + 24 + text->line_height * 10 + GAP;
    log_height = SCREEN_HEIGHT - text->line_height * 2 - 32 - log_y;
    drawLog(canvas, text, log_font, state, log_y, log_height);

    drawFooter(canvas, text);
}
