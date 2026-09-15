#include <dglab/ui/screen.h>

#include <dglab/ui/qr.h>

#include <stdio.h>
#include <string.h>

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720
#define MARGIN 24
#define GAP 16
#define TITLE_HEIGHT 56
#define STATUS_WIDTH 560
#define STATUS_HEIGHT 300
#define LOG_HEIGHT 260
#define LINE_HEIGHT 20
#define LOG_CHARS ((STATUS_WIDTH - 32) / 16)
#define QR_QUIET_ZONE 4
#define QR_MAX_MODULE 10

static const uint32_t kBackground = DGLAB_RGBA(0x10, 0x14, 0x18, 0xFF);
static const uint32_t kPanel = DGLAB_RGBA(0x1C, 0x22, 0x30, 0xFF);
static const uint32_t kPanelBorder = DGLAB_RGBA(0x2E, 0x38, 0x4C, 0xFF);
static const uint32_t kText = DGLAB_RGBA(0xE8, 0xEA, 0xF0, 0xFF);
static const uint32_t kMuted = DGLAB_RGBA(0x9A, 0xA4, 0xB8, 0xFF);
static const uint32_t kAccent = DGLAB_RGBA(0x6F, 0xE3, 0x8A, 0xFF);
static const uint32_t kWarn = DGLAB_RGBA(0xFF, 0xC9, 0x4D, 0xFF);
static const uint32_t kError = DGLAB_RGBA(0xFF, 0x6B, 0x6B, 0xFF);
static const uint32_t kWhite = DGLAB_RGBA(0xFF, 0xFF, 0xFF, 0xFF);
static const uint32_t kBlack = DGLAB_RGBA(0x00, 0x00, 0x00, 0xFF);

static const char* netStateName(u32 state)
{
    switch (state) {
        case DglabNetState_Idle: return "not started";
        case DglabNetState_Listening: return "waiting for the app";
        case DglabNetState_Paired: return "app connected";
        case DglabNetState_Stopped: return "stopped";
        case DglabNetState_Failed: return "FAILED";
        default: return "?";
    }
}

static uint32_t netStateColor(u32 state)
{
    switch (state) {
        case DglabNetState_Paired: return kAccent;
        case DglabNetState_Listening: return kWarn;
        case DglabNetState_Failed: return kError;
        default: return kMuted;
    }
}

static void drawPanel(DglabCanvas* canvas, const DglabFont* font, int x, int y, int width,
    int height, const char* title)
{
    dglabCanvasFill(canvas, x, y, width, height, kPanel);
    dglabCanvasFrame(canvas, x, y, width, height, 2, kPanelBorder);

    if (title)
        dglabCanvasText(canvas, font, x + 12, y + 8, 1, title, kMuted);
}

static void drawLine(DglabCanvas* canvas, const DglabFont* font, int x, int y, const char* label,
    const char* value, uint32_t value_color)
{
    dglabCanvasText(canvas, font, x, y, 1, label, kMuted);
    // The longest label ("controller") is 10 characters; two more leave room for
    // 21 characters of value, which is what "192.168.1.161:9999" needs.
    dglabCanvasText(canvas, font, x + 12 * 16, y, 1, value, value_color);
}

// Wraps at spaces when there is one, and hard breaks otherwise (the socket URL
// has no spaces and still has to fit).
static void drawWrapped(DglabCanvas* canvas, const DglabFont* font, int x, int y, int columns,
    const char* text, uint32_t color)
{
    char line[80];
    size_t length = strlen(text);
    size_t offset = 0;

    if (columns <= 0 || columns >= (int)sizeof(line))
        return;

    while (offset < length) {
        size_t take = length - offset;

        if (take > (size_t)columns) {
            take = (size_t)columns;

            if (offset + take < length && text[offset + take] != ' ') {
                size_t back = take;

                while (back > 0 && text[offset + back] != ' ')
                    back--;

                if (back > (size_t)columns / 3)
                    take = back;
            }
        }

        memcpy(line, text + offset, take);
        line[take] = '\0';

        dglabCanvasText(canvas, font, x, y, 1, line, color);

        y += LINE_HEIGHT;
        offset += take;

        while (offset < length && text[offset] == ' ')
            offset++;
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

static void drawTitle(DglabCanvas* canvas, const DglabFont* font, const DglabScreenState* state)
{
    char right[64];
    char title[80];

    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, TITLE_HEIGHT, kPanel);

    snprintf(title, sizeof(title), "DGLAB-NX   DG-LAB socket server   port %u",
        (unsigned)state->status.port);
    dglabCanvasText(canvas, font, MARGIN, 20, 1, title, kText);

    snprintf(right, sizeof(right), "IPC %u.%u.%u", state->version.major, state->version.minor,
        state->version.patch);

    dglabCanvasText(canvas, font, SCREEN_WIDTH - MARGIN - dglabCanvasTextWidth(font, 1, right), 20,
        1, right, kMuted);
}

static void drawStatus(DglabCanvas* canvas, const DglabFont* font, const DglabScreenState* state)
{
    const DglabNetStatus* status = &state->status;
    int x = MARGIN + 16;
    int y = TITLE_HEIGHT + GAP + 12 + LINE_HEIGHT * 2;
    char buffer[128];

    drawPanel(canvas, font, MARGIN, TITLE_HEIGHT + GAP, STATUS_WIDTH, STATUS_HEIGHT, "server");

    if (!state->status_ok) {
        drawLine(canvas, font, x, y, "status", "IPC call failed", kError);
        return;
    }

    drawLine(canvas, font, x, y, "state", netStateName(status->state),
        netStateColor(status->state));
    y += LINE_HEIGHT;

    if (status->ip_text[0]) {
        snprintf(buffer, sizeof(buffer), "%s:%u", (const char*)status->ip_text,
            (unsigned)status->port);
    } else {
        // The value column only has room for about 19 characters.
        snprintf(buffer, sizeof(buffer), "no address yet");
    }

    drawLine(canvas, font, x, y, "address", buffer, status->ip_text[0] ? kText : kWarn);
    y += LINE_HEIGHT;

    formatShortId((const char*)status->controller_id, buffer, sizeof(buffer));
    drawLine(canvas, font, x, y, "controller", buffer, kText);
    y += LINE_HEIGHT;

    if (status->peer_id[0])
        formatShortId((const char*)status->peer_id, buffer, sizeof(buffer));
    else
        snprintf(buffer, sizeof(buffer), "(not bound)");

    drawLine(canvas, font, x, y, "app id", buffer, status->peer_id[0] ? kAccent : kMuted);
    y += LINE_HEIGHT;

    snprintf(buffer, sizeof(buffer), "%u sessions  %u commands  %u reports",
        (unsigned)status->sessions, (unsigned)status->commands_sent,
        (unsigned)status->reports_received);
    dglabCanvasText(canvas, font, x, y, 1, buffer, kText);
    y += LINE_HEIGHT;

    snprintf(buffer, sizeof(buffer), "%u heartbeats  %u in  %u out",
        (unsigned)status->heartbeats_sent, (unsigned)status->messages_in,
        (unsigned)status->messages_out);
    dglabCanvasText(canvas, font, x, y, 1, buffer, kText);
    y += LINE_HEIGHT;

    snprintf(buffer, sizeof(buffer), "%u/%u (limit %u/%u)", (unsigned)status->app_strength_a,
        (unsigned)status->app_strength_b, (unsigned)status->app_limit_a,
        (unsigned)status->app_limit_b);
    drawLine(canvas, font, x, y, "strength", buffer, kText);
    y += LINE_HEIGHT;

    if (status->app_feedback == DGLAB_NET_FEEDBACK_NONE)
        snprintf(buffer, sizeof(buffer), "-");
    else
        snprintf(buffer, sizeof(buffer), "%u", (unsigned)status->app_feedback);

    drawLine(canvas, font, x, y, "feedback", buffer, kText);
    y += LINE_HEIGHT;

    if (status->last_result)
        snprintf(buffer, sizeof(buffer), "0x%08X", (unsigned)status->last_result);
    else if (status->last_error)
        snprintf(buffer, sizeof(buffer), "protocol error %u", (unsigned)status->last_error);
    else
        snprintf(buffer, sizeof(buffer), "none");

    drawLine(canvas, font, x, y, "last issue", buffer,
        (status->last_result || status->last_error) ? kWarn : kMuted);
    y += LINE_HEIGHT;

    // Raw device value, the same number the App shows: 0..100, above which the
    // official documentation only allows special cases.
    snprintf(buffer, sizeof(buffer), "strength %u of 100", (unsigned)state->test_strength);
    drawLine(canvas, font, x, y, "buttons", buffer, kMuted);

    // While the server is up the sysmodule holds a listening socket, and this
    // console hangs if that happens across a sleep. Say so on screen.
    if (status->state == DglabNetState_Listening || status->state == DglabNetState_Paired) {
        y += LINE_HEIGHT;
        drawWrapped(canvas, font, x, y, (STATUS_WIDTH - 32) / 16,
            "do not sleep the console while the server runs: press Y first.", kWarn);
    }
}

static void drawQr(DglabCanvas* canvas, const DglabFont* font, const DglabScreenState* state)
{
    // Encoding costs a handful of matrix builds, so the result is cached until
    // the payload changes.
    static char cached_url[DGLAB_NET_QR_MAX];
    static DglabQrCode cached_code;
    static bool cached = false;
    int panel_x = MARGIN * 2 + STATUS_WIDTH;
    int panel_y = TITLE_HEIGHT + GAP;
    int panel_w = SCREEN_WIDTH - panel_x - MARGIN;
    int panel_h = SCREEN_HEIGHT - panel_y - TITLE_HEIGHT - GAP;
    int total;
    int module;
    int qr_size;

    drawPanel(canvas, font, panel_x, panel_y, panel_w, panel_h, "scan this with the DG-LAB app");

    if (state->status.state != DglabNetState_Listening &&
        state->status.state != DglabNetState_Paired) {
        char message[192];

        // The server is not running yet, which is a different situation from a
        // console that has no LAN address.
        snprintf(message, sizeof(message),
            "the socket server is not running. press A to start it on port %u (it stops itself "
            "after 55 s so the console can sleep).",
            (unsigned)state->status.port);
        drawWrapped(canvas, font, panel_x + 20, panel_y + 60, (panel_w - 40) / 16, message, kWarn);
        return;
    }

    if (!state->url_ok || !state->url || state->url[0] == '\0') {
        // Wrapped, because the panel is 41 characters wide and the message is
        // longer than that.
        drawWrapped(canvas, font, panel_x + 20, panel_y + 60, (panel_w - 40) / 16,
            "no QR code yet: the server has no LAN address. Join the same Wi-Fi network as the "
            "phone.",
            kWarn);
        return;
    }

    if (!cached || strcmp(cached_url, state->url) != 0) {
        if (!dglabQrEncodeString(&cached_code, state->url, DglabQrEcc_M)) {
            dglabCanvasText(canvas, font, panel_x + 20, panel_y + 60, 1,
                "the socket url does not fit a QR code", kError);
            return;
        }

        snprintf(cached_url, sizeof(cached_url), "%s", state->url);
        cached = true;
    }

    total = (int)cached_code.size + QR_QUIET_ZONE * 2;

    // The payload is long, so it is wrapped under the symbol: how much room it
    // needs decides how large the modules can be.
    {
        int columns = (panel_w - 32) / 16;
        int text_lines = (int)((strlen(state->url) + (size_t)columns - 1) / (size_t)columns);
        int reserved = text_lines * LINE_HEIGHT + 12;
        int available = panel_h - 40 - reserved - 12;

        if (columns <= 0) {
            dglabCanvasText(canvas, font, panel_x + 20, panel_y + 60, 1,
                "the socket url does not fit a QR code", kError);
            return;
        }

        module = available / total;
    }

    if (module > QR_MAX_MODULE)
        module = QR_MAX_MODULE;

    if (module < 1)
        module = 1;

    qr_size = total * module;

    dglabCanvasQr(canvas, &cached_code, panel_x + (panel_w - qr_size) / 2, panel_y + 40, module,
        QR_QUIET_ZONE, kBlack, kWhite);

    drawWrapped(canvas, font, panel_x + 16, panel_y + 40 + qr_size + 16,
        (panel_w - 32) / 16, state->url, kMuted);
}

static void drawLog(DglabCanvas* canvas, const DglabFont* font, const DglabScreenState* state)
{
    int x = MARGIN;
    int y = TITLE_HEIGHT + GAP + STATUS_HEIGHT + GAP;
    char line[LOG_CHARS + 1];
    int text_y = y + 12 + LINE_HEIGHT;

    drawPanel(canvas, font, x, y, STATUS_WIDTH, LOG_HEIGHT, "sysmodule log");

    for (int i = 0; i < state->log_count && i < DGLAB_SCREEN_LOG_LINES; i++) {
        const char* text = state->log_lines[i];
        size_t length = strlen(text);

        if (length > LOG_CHARS)
            length = LOG_CHARS;

        memcpy(line, text, length);
        line[length] = '\0';

        dglabCanvasText(canvas, font, x + 16, text_y + i * LINE_HEIGHT, 1, line, kMuted);
    }
}

static void drawFooter(DglabCanvas* canvas, const DglabFont* font)
{
    // Two lines: one line of hints is wider than the screen at 16 pixels per
    // character, which used to push the trailing "+ exit" off the edge.
    dglabCanvasText(canvas, font, MARGIN, SCREEN_HEIGHT - 56, 1,
        "A start    Y stop    X strength    B clear", kText);
    dglabCanvasText(canvas, font, MARGIN, SCREEN_HEIGHT - 32, 1,
        "ZL test    L/R value    - BLE poc    + exit", kText);
}

void dglabScreenDraw(DglabCanvas* canvas, const DglabFont* font, const DglabScreenState* state)
{
    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, kBackground);

    drawTitle(canvas, font, state);
    drawStatus(canvas, font, state);
    drawQr(canvas, font, state);
    drawLog(canvas, font, state);
    drawFooter(canvas, font);
}
