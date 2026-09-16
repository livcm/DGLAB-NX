#pragma once

// Minimal JSON reader for the language files (docs/nro-ui.md).
//
// It is not a general purpose parser: it reads what a hand written translation
// file holds - objects, arrays, strings, numbers, true/false/null - and points
// at the line of the first mistake instead of guessing what was meant. It never
// allocates and holds no state outside the text it was given.

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    DglabJsonToken_None = 0, ///< nothing was read, or the reader is done failing
    DglabJsonToken_BeginObject, ///< {
    DglabJsonToken_EndObject,   ///< }
    DglabJsonToken_BeginArray,  ///< [
    DglabJsonToken_EndArray,    ///< ]
    DglabJsonToken_Colon,       ///< :
    DglabJsonToken_Comma,       ///< ,
    DglabJsonToken_String,      ///< "..." (the text is in dglabJsonText)
    DglabJsonToken_Number,
    DglabJsonToken_True,
    DglabJsonToken_False,
    DglabJsonToken_Null,
    DglabJsonToken_EndOfInput,
} DglabJsonToken;

typedef struct {
    const char* cursor;
    const char* end;
    unsigned line; ///< line the next token starts on, 1 based
    unsigned token_line;
    const char* error; ///< message for the first mistake, NULL while the file is valid
    unsigned error_line;
    char* buffer; ///< holds the decoded text of one token
    size_t buffer_size;
    size_t length; ///< length of that text
} DglabJsonReader;

/// Prepares `reader` to read `size` bytes at `text`. `buffer` holds the decoded
/// text of one token and has to stay valid (and untouched) until the caller is
/// done with the reader, so it must not be the text being read.
void dglabJsonInit(DglabJsonReader* reader, const char* text, size_t size, char* buffer,
    size_t buffer_size);

/// Reads the next token. After the first mistake every call returns
/// DglabJsonToken_None, so a caller that keeps looping ends up one step later.
DglabJsonToken dglabJsonNext(DglabJsonReader* reader);

/// Skips the value that `first` (a token just read from this reader) starts,
/// together with everything nested in it. For members this reader has no use
/// for. Returns false when the value is not well formed.
bool dglabJsonSkipValue(DglabJsonReader* reader, DglabJsonToken first);

/// NUL terminated text of the last String/Number token. Valid until the next
/// dglabJsonNext call.
const char* dglabJsonText(const DglabJsonReader* reader);

/// Length of that text. An escape such as \u0000 can put a zero byte in it, so
/// a caller that stores the text uses this instead of strlen.
size_t dglabJsonTextLength(const DglabJsonReader* reader);

/// Line the last token started on, 1 based.
unsigned dglabJsonTokenLine(const DglabJsonReader* reader);

/// Line the first mistake happened on, 1 based.
unsigned dglabJsonErrorLine(const DglabJsonReader* reader);

/// Message for the first mistake, e.g. "unterminated string"; NULL while the
/// file reads cleanly.
const char* dglabJsonError(const DglabJsonReader* reader);
