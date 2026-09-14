#include <dglab/net/dglab_socket.h>

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Minimal JSON object reading
// ---------------------------------------------------------------------------

// Finds the value of a top level string key. Returns false when the key is not
// present. `out` receives the unescaped value.
static bool jsonFindString(const char* text, size_t size, const char* key, char* out,
    size_t out_size)
{
    size_t key_len = strlen(key);
    size_t position = 0;

    while (position + key_len + 2 < size) {
        if (text[position] != '"') {
            position++;
            continue;
        }

        if (memcmp(text + position + 1, key, key_len) != 0 ||
            text[position + 1 + key_len] != '"') {
            // Skip this string.
            position++;

            while (position < size && text[position] != '"')
                position++;

            position++;
            continue;
        }

        size_t cursor = position + key_len + 2;

        while (cursor < size && (text[cursor] == ' ' || text[cursor] == '\t'))
            cursor++;

        if (cursor >= size || text[cursor] != ':')
            return false;

        cursor++;

        while (cursor < size && (text[cursor] == ' ' || text[cursor] == '\t'))
            cursor++;

        if (cursor >= size || text[cursor] != '"')
            return false;

        cursor++;

        size_t written = 0;

        while (cursor < size && text[cursor] != '"') {
            char c = text[cursor++];

            if (c == '\\') {
                if (cursor >= size)
                    return false;

                char escaped = text[cursor++];

                switch (escaped) {
                    case '"': c = '"'; break;
                    case '\\': c = '\\'; break;
                    case '/': c = '/'; break;
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    default: return false; // \uXXXX and friends are not needed here
                }
            }

            if (written + 1 >= out_size)
                return false;

            out[written++] = c;
        }

        out[written] = '\0';
        return true;
    }

    return false;
}

static DglabSocketType typeFromName(const char* name)
{
    if (strcmp(name, "heartbeat") == 0) return DglabSocketType_Heartbeat;
    if (strcmp(name, "bind") == 0) return DglabSocketType_Bind;
    if (strcmp(name, "msg") == 0) return DglabSocketType_Msg;
    if (strcmp(name, "break") == 0) return DglabSocketType_Break;
    if (strcmp(name, "error") == 0) return DglabSocketType_Error;

    return DglabSocketType_Unknown;
}

const char* dglabSocketTypeName(DglabSocketType type)
{
    switch (type) {
        case DglabSocketType_Heartbeat: return "heartbeat";
        case DglabSocketType_Bind: return "bind";
        case DglabSocketType_Msg: return "msg";
        case DglabSocketType_Break: return "break";
        case DglabSocketType_Error: return "error";
        default: return "";
    }
}

bool dglabSocketParseMessage(const char* text, size_t size, DglabSocketMessage* out)
{
    char type[24];

    if (size < 2 || text[0] != '{')
        return false;

    memset(out, 0, sizeof(*out));
    out->code = -1;

    if (!jsonFindString(text, size, "type", type, sizeof(type)))
        return false;

    out->type = typeFromName(type);

    jsonFindString(text, size, "clientId", out->client_id, sizeof(out->client_id));
    jsonFindString(text, size, "targetId", out->target_id, sizeof(out->target_id));

    if (jsonFindString(text, size, "message", out->message, sizeof(out->message))) {
        // A message that is only digits is one of the protocol's status codes.
        bool digits = out->message[0] != '\0';
        int value = 0;

        for (const char* p = out->message; *p; p++) {
            if (*p < '0' || *p > '9') {
                digits = false;
                break;
            }

            value = value * 10 + (*p - '0');
        }

        if (digits)
            out->code = value;
    }

    return true;
}

static bool appendLiteral(char* out, size_t out_size, size_t* written, const char* text)
{
    size_t len = strlen(text);

    if (*written + len + 1 > out_size)
        return false;

    memcpy(out + *written, text, len);
    *written += len;

    return true;
}

// The pulse command carries double quotes ("pulse-A:[\"0A0A...\"]"), so every
// field has to be escaped before it is embedded in the envelope. Control
// characters never appear in this protocol and are rejected instead of being
// replaced, so a caller cannot silently send something the App cannot read.
static bool appendEscaped(char* out, size_t out_size, size_t* written, const char* value)
{
    for (const char* cursor = value; *cursor; cursor++) {
        char c = *cursor;

        if (c == '"' || c == '\\') {
            if (*written + 3 > out_size)
                return false;

            out[(*written)++] = '\\';
            out[(*written)++] = c;
        } else if ((unsigned char)c < 0x20) {
            return false;
        } else {
            if (*written + 2 > out_size)
                return false;

            out[(*written)++] = c;
        }
    }

    return true;
}

size_t dglabSocketBuildMessage(char* out, size_t out_size, const char* type, const char* client_id,
    const char* target_id, const char* message)
{
    size_t written = 0;

    if (!out || out_size == 0)
        return 0;

    out[0] = '\0';

    if (!appendLiteral(out, out_size, &written, "{\"type\":\"") ||
        !appendEscaped(out, out_size, &written, type) ||
        !appendLiteral(out, out_size, &written, "\",\"clientId\":\"") ||
        !appendEscaped(out, out_size, &written, client_id ? client_id : "") ||
        !appendLiteral(out, out_size, &written, "\",\"targetId\":\"") ||
        !appendEscaped(out, out_size, &written, target_id ? target_id : "") ||
        !appendLiteral(out, out_size, &written, "\",\"message\":\"") ||
        !appendEscaped(out, out_size, &written, message ? message : "") ||
        !appendLiteral(out, out_size, &written, "\"}")) {
        out[0] = '\0';
        return 0;
    }

    out[written] = '\0';

    return written;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

size_t dglabSocketBuildStrength(char* out, size_t out_size, DglabSocketChannel channel,
    DglabSocketStrengthOp operation, int value)
{
    int written = snprintf(out, out_size, "strength-%d+%d+%d", (int)channel, (int)operation, value);

    if (written <= 0 || (size_t)written >= out_size)
        return 0;

    return (size_t)written;
}

size_t dglabSocketBuildClear(char* out, size_t out_size, DglabSocketChannel channel)
{
    int written = snprintf(out, out_size, "clear-%d", (int)channel);

    if (written <= 0 || (size_t)written >= out_size)
        return 0;

    return (size_t)written;
}

size_t dglabSocketBuildPulse(char* out, size_t out_size, DglabSocketChannel channel,
    const char* const* pulse_hex, size_t count)
{
    const char letter = (channel == DglabSocketChannel_A) ? 'A' : 'B';
    size_t written = 0;
    int result;

    result = snprintf(out + written, out_size - written, "pulse-%c:[", letter);

    if (result <= 0)
        return 0;

    written += (size_t)result;

    for (size_t i = 0; i < count; i++) {
        if (written + 2 >= out_size)
            return 0;

        result = snprintf(out + written, out_size - written, "%s\"%s\"",
            (i == 0) ? "" : ",", pulse_hex[i]);

        if (result <= 0 || (size_t)result >= out_size - written)
            return 0;

        written += (size_t)result;
    }

    if (written + 1 >= out_size)
        return 0;

    out[written++] = ']';
    out[written] = '\0';

    return written;
}

// ---------------------------------------------------------------------------
// Reports from the App
// ---------------------------------------------------------------------------

static bool parseIntegers(const char* text, int* values, size_t count)
{
    const char* cursor = text;

    for (size_t i = 0; i < count; i++) {
        // Values are separated by '+'; the first one has no separator in front.
        if (i > 0) {
            if (*cursor != '+')
                return false;

            cursor++;
        }

        bool negative = false;

        if (*cursor == '-') {
            negative = true;
            cursor++;
        }

        if (*cursor < '0' || *cursor > '9')
            return false;

        int value = 0;

        while (*cursor >= '0' && *cursor <= '9')
            value = value * 10 + (*cursor++ - '0');

        values[i] = negative ? -value : value;
    }

    return *cursor == '\0';
}

bool dglabSocketParseStrengthReport(const char* message, DglabSocketStrengthData* out)
{
    static const char prefix[] = "strength-";
    int values[4];

    if (strncmp(message, prefix, sizeof(prefix) - 1) != 0)
        return false;

    if (!parseIntegers(message + sizeof(prefix) - 1, values, 4))
        return false;

    out->a = values[0];
    out->b = values[1];
    out->a_limit = values[2];
    out->b_limit = values[3];

    return true;
}

bool dglabSocketParseFeedback(const char* message, int* out_button)
{
    static const char prefix[] = "feedback-";
    int values[1];

    if (strncmp(message, prefix, sizeof(prefix) - 1) != 0)
        return false;

    if (!parseIntegers(message + sizeof(prefix) - 1, values, 1))
        return false;

    *out_button = values[0];

    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void dglabSocketEncodePulseHex(const DglabCoyoteV3WaveformSlot* slots, char* out)
{
    static const char digits[] = "0123456789ABCDEF";
    uint8_t bytes[8];

    for (size_t i = 0; i < DGLAB_COYOTE_V3_WAVEFORM_SLOTS; i++)
        bytes[i] = slots[i].frequency;

    for (size_t i = 0; i < DGLAB_COYOTE_V3_WAVEFORM_SLOTS; i++)
        bytes[DGLAB_COYOTE_V3_WAVEFORM_SLOTS + i] = slots[i].strength;

    for (size_t i = 0; i < sizeof(bytes); i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0F];
    }

    out[sizeof(bytes) * 2] = '\0';
}

size_t dglabSocketBuildQrUrl(char* out, size_t out_size, const char* ws_uri,
    const char* client_id)
{
    int written = snprintf(out, out_size,
        "https://www.dungeon-lab.com/app-download.php#DGLAB-SOCKET#%s/%s", ws_uri, client_id);

    if (written <= 0 || (size_t)written >= out_size)
        return 0;

    return (size_t)written;
}
