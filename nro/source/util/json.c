#include <dglab/util/json.h>

#include <string.h>

// The reader walks the text once, token by token, and remembers only where it
// is and what went wrong first. Every message it produces is a pointer to a
// literal, so nothing here owns memory: the caller keeps the text, the decoded
// token buffer and the reader itself.

static void setError(DglabJsonReader* reader, const char* message)
{
    if (reader->error == NULL) {
        reader->error = message;
        reader->error_line = reader->token_line;
    }
}

static bool atEnd(const DglabJsonReader* reader)
{
    return reader->cursor >= reader->end;
}

static char peek(const DglabJsonReader* reader)
{
    return atEnd(reader) ? '\0' : *reader->cursor;
}

// Whitespace between tokens; newlines also move the line counter, which is what
// makes an error message useful in a file a person edited by hand.
static void skipSpace(DglabJsonReader* reader)
{
    while (!atEnd(reader)) {
        char c = *reader->cursor;

        if (c == '\n') {
            reader->line++;
        } else if (c != ' ' && c != '\t' && c != '\r') {
            break;
        }

        reader->cursor++;
    }
}

static bool appendByte(DglabJsonReader* reader, unsigned value)
{
    if (reader->length + 1 >= reader->buffer_size)
        return false;

    reader->buffer[reader->length++] = (char)value;
    reader->buffer[reader->length] = '\0';
    return true;
}

// \uXXXX becomes the UTF-8 bytes for that code point, so a file written with
// escapes reads the same as one written with the characters themselves.
static bool readHex4(DglabJsonReader* reader, unsigned* out)
{
    unsigned value = 0;

    for (int i = 0; i < 4; i++) {
        char c = peek(reader);
        unsigned digit;

        if (c >= '0' && c <= '9')
            digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = (unsigned)(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F')
            digit = (unsigned)(c - 'A') + 10u;
        else
            return false;

        value = value * 16u + digit;
        reader->cursor++;
    }

    *out = value;
    return true;
}

static bool appendCodePoint(DglabJsonReader* reader, unsigned code)
{
    if (code < 0x80u)
        return appendByte(reader, code);

    if (code < 0x800u)
        return appendByte(reader, 0xC0u | (code >> 6)) && appendByte(reader, 0x80u | (code & 0x3Fu));

    if (code < 0x10000u) {
        return appendByte(reader, 0xE0u | (code >> 12)) &&
               appendByte(reader, 0x80u | ((code >> 6) & 0x3Fu)) &&
               appendByte(reader, 0x80u | (code & 0x3Fu));
    }

    return appendByte(reader, 0xF0u | (code >> 18)) &&
           appendByte(reader, 0x80u | ((code >> 12) & 0x3Fu)) &&
           appendByte(reader, 0x80u | ((code >> 6) & 0x3Fu)) &&
           appendByte(reader, 0x80u | (code & 0x3Fu));
}

// One escape sequence, the backslash already consumed.
static bool readEscape(DglabJsonReader* reader)
{
    char c = peek(reader);
    unsigned code;

    reader->cursor++;

    switch (c) {
        case '"': return appendByte(reader, '"');
        case '\\': return appendByte(reader, '\\');
        case '/': return appendByte(reader, '/');
        case 'b': return appendByte(reader, '\b');
        case 'f': return appendByte(reader, '\f');
        case 'n': return appendByte(reader, '\n');
        case 'r': return appendByte(reader, '\r');
        case 't': return appendByte(reader, '\t');
        default: break;
    }

    if (c != 'u') {
        setError(reader, "unknown escape");
        return false;
    }

    if (!readHex4(reader, &code)) {
        setError(reader, "bad \\u escape");
        return false;
    }

    // A character outside the basic plane is written as two escapes; a lone
    // half of a pair is a mistake rather than something to render.
    if (code >= 0xD800u && code <= 0xDBFFu) {
        unsigned low;

        if (peek(reader) != '\\' || reader->cursor + 1 >= reader->end ||
            reader->cursor[1] != 'u') {
            setError(reader, "surrogate pair cut short");
            return false;
        }

        reader->cursor += 2;

        if (!readHex4(reader, &low) || low < 0xDC00u || low > 0xDFFFu) {
            setError(reader, "bad surrogate pair");
            return false;
        }

        code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
    } else if (code >= 0xDC00u && code <= 0xDFFFu) {
        setError(reader, "lone surrogate");
        return false;
    }

    return appendCodePoint(reader, code);
}

static bool readString(DglabJsonReader* reader)
{
    reader->length = 0;

    if (reader->buffer_size == 0) {
        setError(reader, "no room for the text");
        return false;
    }

    reader->buffer[0] = '\0';
    reader->cursor++; // the opening quote

    while (true) {
        char c;

        if (atEnd(reader)) {
            setError(reader, "unterminated string");
            return false;
        }

        c = *reader->cursor++;

        if (c == '"')
            return true;

        if ((unsigned char)c < 0x20u) {
            setError(reader, "raw control character");
            return false;
        }

        if (c == '\\') {
            if (!readEscape(reader))
                return false;
            continue;
        }

        if (!appendByte(reader, (unsigned char)c)) {
            setError(reader, "text too long");
            return false;
        }
    }
}

static bool isNumberChar(char c)
{
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

static bool readNumber(DglabJsonReader* reader)
{
    reader->length = 0;
    reader->buffer[0] = '\0';

    while (!atEnd(reader) && isNumberChar(*reader->cursor)) {
        if (!appendByte(reader, (unsigned char)*reader->cursor)) {
            setError(reader, "number too long");
            return false;
        }

        reader->cursor++;
    }

    return true;
}

// "true", "false", "null": the rest of the literal has to be there.
static bool readKeyword(DglabJsonReader* reader, const char* keyword)
{
    size_t length = strlen(keyword);

    if ((size_t)(reader->end - reader->cursor) < length || memcmp(reader->cursor, keyword, length) != 0)
        return false;

    reader->cursor += length;
    return true;
}

void dglabJsonInit(DglabJsonReader* reader, const char* text, size_t size, char* buffer,
    size_t buffer_size)
{
    reader->cursor = text;
    reader->end = text + size;
    reader->line = 1;
    reader->token_line = 1;
    reader->error = NULL;
    reader->error_line = 0;
    reader->buffer = buffer;
    reader->buffer_size = buffer_size;
    reader->length = 0;

    if (buffer_size > 0)
        buffer[0] = '\0';
}

DglabJsonToken dglabJsonNext(DglabJsonReader* reader)
{
    char c;

    if (reader->error != NULL)
        return DglabJsonToken_None;

    skipSpace(reader);
    reader->token_line = reader->line;
    reader->length = 0;

    if (reader->buffer_size > 0)
        reader->buffer[0] = '\0';

    if (atEnd(reader))
        return DglabJsonToken_EndOfInput;

    c = *reader->cursor;

    switch (c) {
        case '{':
            reader->cursor++;
            return DglabJsonToken_BeginObject;
        case '}':
            reader->cursor++;
            return DglabJsonToken_EndObject;
        case '[':
            reader->cursor++;
            return DglabJsonToken_BeginArray;
        case ']':
            reader->cursor++;
            return DglabJsonToken_EndArray;
        case ':':
            reader->cursor++;
            return DglabJsonToken_Colon;
        case ',':
            reader->cursor++;
            return DglabJsonToken_Comma;
        case '"':
            return readString(reader) ? DglabJsonToken_String : DglabJsonToken_None;
        case 't':
            if (readKeyword(reader, "true"))
                return DglabJsonToken_True;
            setError(reader, "bad literal");
            return DglabJsonToken_None;
        case 'f':
            if (readKeyword(reader, "false"))
                return DglabJsonToken_False;
            setError(reader, "bad literal");
            return DglabJsonToken_None;
        case 'n':
            if (readKeyword(reader, "null"))
                return DglabJsonToken_Null;
            setError(reader, "bad literal");
            return DglabJsonToken_None;
        default:
            break;
    }

    if (isNumberChar(c)) {
        return readNumber(reader) ? DglabJsonToken_Number : DglabJsonToken_None;
    }

    setError(reader, "unexpected character");
    return DglabJsonToken_None;
}

bool dglabJsonSkipValue(DglabJsonReader* reader, DglabJsonToken first)
{
    int depth = 0;
    DglabJsonToken token = first;

    while (true) {
        switch (token) {
            case DglabJsonToken_BeginObject:
            case DglabJsonToken_BeginArray:
                depth++;
                break;
            case DglabJsonToken_EndObject:
            case DglabJsonToken_EndArray:
                depth--;
                if (depth <= 0)
                    return true;
                break;
            case DglabJsonToken_String:
            case DglabJsonToken_Number:
            case DglabJsonToken_True:
            case DglabJsonToken_False:
            case DglabJsonToken_Null:
                if (depth == 0)
                    return true;
                break;
            case DglabJsonToken_EndOfInput:
                setError(reader, "unexpected end of file");
                return false;
            default:
                // A colon or comma in the middle of a value that is being
                // skipped belongs to the container, and the containers are what
                // the depth counter follows, so both are stepped over.
                break;
        }

        token = dglabJsonNext(reader);

        if (token == DglabJsonToken_None)
            return false;
    }
}

const char* dglabJsonText(const DglabJsonReader* reader)
{
    return reader->buffer;
}

size_t dglabJsonTextLength(const DglabJsonReader* reader)
{
    return reader->length;
}

unsigned dglabJsonTokenLine(const DglabJsonReader* reader)
{
    return reader->token_line;
}

unsigned dglabJsonErrorLine(const DglabJsonReader* reader)
{
    return reader->error_line;
}

const char* dglabJsonError(const DglabJsonReader* reader)
{
    return reader->error;
}
