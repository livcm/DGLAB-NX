// Host side tests for the NRO's QR encoder.
//
// The interesting cases are checked against golden matrices produced by an
// independent implementation (macOS CoreImage, see tools/gen_reference.m): the
// version, the error correction blocks, the mask choice and the module layout
// all have to match exactly, so a wrong table or a wrong placement shows up as
// a diff instead of as an unscannable code.
//
// Run with: make -C tests/qr

#include <dglab/ui/qr.h>

#include <stdio.h>
#include <stdlib.h>
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

// ---------------------------------------------------------------------------
// Golden files
// ---------------------------------------------------------------------------

typedef struct {
    char text[512];
    char level[8];
    int size;
    uint8_t modules[DGLAB_QR_MAX_SIZE][DGLAB_QR_MAX_SIZE];
} Golden;

static bool loadGolden(const char* path, Golden* out)
{
    FILE* file = fopen(path, "r");
    char line[2 * DGLAB_QR_MAX_SIZE];
    int row = 0;

    if (file == NULL) {
        printf("FAIL cannot open %s\n", path);
        g_failures++;
        return false;
    }

    memset(out, 0, sizeof(*out));

    while (fgets(line, sizeof(line), file) != NULL) {
        size_t len = strlen(line);

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (line[0] == '#') {
            if (sscanf(line, "# text: %511[^\n]", out->text) == 1)
                continue;

            if (sscanf(line, "# level: %7s", out->level) == 1)
                continue;

            sscanf(line, "# size: %d", &out->size);
            continue;
        }

        if (len == 0)
            continue;

        if (row >= (int)DGLAB_QR_MAX_SIZE || (int)len < out->size) {
            fclose(file);
            return false;
        }

        for (int col = 0; col < out->size; col++)
            out->modules[row][col] = (uint8_t)(line[col] == '1');

        row++;
    }

    fclose(file);

    return row == out->size && out->size >= 21;
}

static DglabQrEcc levelFromName(const char* name)
{
    switch (name[0]) {
        case 'L': return DglabQrEcc_L;
        case 'Q': return DglabQrEcc_Q;
        case 'H': return DglabQrEcc_H;
        default: return DglabQrEcc_M;
    }
}

static void compareGolden(const char* name)
{
    char path[256];
    Golden golden;
    DglabQrCode code;

    snprintf(path, sizeof(path), "golden/%s.txt", name);

    if (!loadGolden(path, &golden))
        return;

    CHECK(dglabQrEncodeString(&code, golden.text, levelFromName(golden.level)));
    CHECK(code.size == golden.size);

    if (code.size != golden.size) {
        printf("     %s: size %u, reference %d\n", name, code.size, golden.size);
        g_failures += 0;
        return;
    }

    {
        int differences = 0;

        for (int row = 0; row < golden.size; row++) {
            for (int col = 0; col < golden.size; col++) {
                if (code.modules[row][col] != golden.modules[row][col])
                    differences++;
            }
        }

        g_checks++;

        if (differences != 0) {
            printf("FAIL %s: %d modules differ from the reference\n", name, differences);

            for (int row = 0; row < golden.size && row < 8; row++) {
                printf("     ours ");
                for (int col = 0; col < golden.size; col++)
                    printf("%c", code.modules[row][col] ? '1' : '0');
                printf("\n     ref  ");
                for (int col = 0; col < golden.size; col++)
                    printf("%c", golden.modules[row][col] ? '1' : '0');
                printf("\n");
            }

            g_failures++;
        }
    }
}

// ---------------------------------------------------------------------------
// Structural checks
// ---------------------------------------------------------------------------

static void testVersionSelection(void)
{
    CHECK(dglabQrFindVersion(5, DglabQrEcc_M) == 1);
    CHECK(dglabQrFindVersion(14, DglabQrEcc_M) == 1);
    CHECK(dglabQrFindVersion(15, DglabQrEcc_M) == 2);

    // Level H holds fewer bytes than level L for the same version.
    CHECK(dglabQrFindVersion(20, DglabQrEcc_H) > dglabQrFindVersion(20, DglabQrEcc_L));

    // The socket URL this project generates fits with room to spare.
    CHECK(dglabQrFindVersion(114, DglabQrEcc_M) != 0);
    CHECK(dglabQrFindVersion(114, DglabQrEcc_M) <= 8);

    // Way past the supported range.
    CHECK(dglabQrFindVersion(400, DglabQrEcc_H) == 0);
    CHECK(!dglabQrEncode(NULL, "x", 1, DglabQrEcc_M));
    CHECK(!dglabQrEncodeString(NULL, "x", DglabQrEcc_M));
}

static void testFinderPatterns(void)
{
    DglabQrCode code;

    CHECK(dglabQrEncodeString(&code, "DGLAB-NX", DglabQrEcc_M));

    // The three finder patterns are always there, with their separators light.
    {
        static const int corners[3][2] = { { 0, 0 }, { 0, -1 }, { -1, 0 } };

        for (int i = 0; i < 3; i++) {
            int base_row = corners[i][0] == -1 ? code.size - 7 : 0;
            int base_col = corners[i][1] == -1 ? code.size - 7 : 0;

            CHECK(code.modules[base_row][base_col] == 1);
            CHECK(code.modules[base_row + 1][base_col + 1] == 0);
            CHECK(code.modules[base_row + 3][base_col + 3] == 1);
        }
    }

    // The dark module.
    CHECK(code.modules[4 * code.version + 9][8] == 1);

    // Timing patterns alternate.
    for (int i = 8; i < code.size - 8; i++) {
        CHECK(code.modules[6][i] == (uint8_t)((i % 2 == 0) ? 1 : 0));
        CHECK(code.modules[i][6] == (uint8_t)((i % 2 == 0) ? 1 : 0));
    }
}

static void testCapacityBoundaries(void)
{
    // A payload that still fits must succeed.
    {
        char text[DGLAB_QR_MAX_SIZE];
        DglabQrCode code;

        memset(text, 'A', sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';

        CHECK(dglabQrEncodeString(&code, text, DglabQrEcc_H));
    }

    // Far past the largest supported symbol the call must fail instead of
    // scribbling outside the matrix.
    {
        char* text = malloc(4096);
        DglabQrCode code;

        memset(text, 'A', 4095);
        text[4095] = '\0';

        CHECK(!dglabQrEncodeString(&code, text, DglabQrEcc_L));
        free(text);
    }
}

int main(void)
{
    compareGolden("byte_v2_m");
    compareGolden("byte_h");
    compareGolden("socket_url_m");
    compareGolden("long_l");

    testVersionSelection();
    testFinderPatterns();
    testCapacityBoundaries();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
