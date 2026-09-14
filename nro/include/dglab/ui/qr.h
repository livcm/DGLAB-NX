#pragma once

// Minimal QR code encoder (ISO/IEC 18004), byte mode, no external dependency.
//
// The NRO needs a QR code because the DG-LAB App can only join by scanning one,
// and devkitPro ships no QR library. Only what this project needs is
// implemented: byte mode, error correction levels L/M/Q/H and versions 1..10.
// Version 10 at level M holds 213 bytes, well past the socket URL, so the
// matrix stays small (57x57 modules).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Version 10 is 57x57 modules.
#define DGLAB_QR_MAX_SIZE 57u

typedef enum {
    DglabQrEcc_L = 0,
    DglabQrEcc_M = 1,
    DglabQrEcc_Q = 2,
    DglabQrEcc_H = 3,
} DglabQrEcc;

typedef struct {
    uint8_t modules[DGLAB_QR_MAX_SIZE][DGLAB_QR_MAX_SIZE]; // 1 = dark
    uint8_t size;    // modules per side, 21..57
    uint8_t version; // 1..10
    uint8_t mask;    // mask pattern that was selected (0..7)
    DglabQrEcc ecc;
} DglabQrCode;

// Smallest version that fits size bytes at the given level, or 0 when even the
// largest supported version is too small.
uint8_t dglabQrFindVersion(size_t size, DglabQrEcc ecc);

// Encodes size bytes from data. Returns false when it does not fit.
bool dglabQrEncode(DglabQrCode* out, const void* data, size_t size, DglabQrEcc ecc);

// Convenience wrapper for a NUL terminated string.
bool dglabQrEncodeString(DglabQrCode* out, const char* text, DglabQrEcc ecc);
