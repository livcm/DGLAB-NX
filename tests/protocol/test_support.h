#pragma once

// Shared helpers for the host side protocol tests.
//
// Header only and static inline so that every test file stays a single
// translation unit with its own main().

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

#define CHECK(condition)                                                \
    do {                                                                \
        g_checks++;                                                     \
        if (!(condition)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            g_failures++;                                               \
        }                                                               \
    } while (0)

#define CHECK_U8(actual, expected)                                          \
    do {                                                                    \
        unsigned int check_actual = (unsigned int)(actual);                 \
        unsigned int check_expected = (unsigned int)(expected);             \
        g_checks++;                                                         \
        if (check_actual != check_expected) {                               \
            printf("FAIL %s:%d: %s = 0x%02X, expected 0x%02X\n", __FILE__,  \
                __LINE__, #actual, check_actual, check_expected);           \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static inline bool bytesFromHex(const char* hex, uint8_t* out, size_t out_size)
{
    if (strlen(hex) != out_size * 2)
        return false;

    for (size_t i = 0; i < out_size; i++) {
        unsigned int value = 0;
        if (sscanf(hex + i * 2, "%2x", &value) != 1)
            return false;
        out[i] = (uint8_t)value;
    }

    return true;
}

static inline void printBytes(const char* label, const uint8_t* bytes, size_t size)
{
    printf("  %s =", label);
    for (size_t i = 0; i < size; i++)
        printf(" %02X", bytes[i]);
    printf("\n");
}

static inline void expectByteArray(const char* name, const uint8_t* actual,
    const uint8_t* expected, size_t size)
{
    g_checks++;
    if (memcmp(actual, expected, size) != 0) {
        printf("FAIL %s: packet mismatch\n", name);
        printBytes("actual  ", actual, size);
        printBytes("expected", expected, size);
        g_failures++;
    }
}

// Compares against a hex string, used for values copied verbatim from the
// official documentation.
static inline void expectBytes(const char* name, const uint8_t* actual,
    const char* expected_hex, size_t size)
{
    uint8_t expected[64];

    if (size > sizeof(expected) || !bytesFromHex(expected_hex, expected, size)) {
        printf("FAIL %s: malformed test vector '%s'\n", name, expected_hex);
        g_failures++;
        return;
    }

    expectByteArray(name, actual, expected, size);
}

static inline int testFinish(void)
{
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
