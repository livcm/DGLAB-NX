#include <dglab/ui/qr.h>

#include <string.h>

// ---------------------------------------------------------------------------
// Version tables (ISO/IEC 18004)
// ---------------------------------------------------------------------------

// Error correction blocks per version and level. Each entry holds the number of
// error correction codewords per block plus up to two groups of blocks, because
// the later versions mix blocks with different data sizes. A block's total size
// is its data codewords plus ec_codewords.
typedef struct {
    uint8_t ec_codewords;
    uint8_t group1_blocks;
    uint8_t group1_data;
    uint8_t group2_blocks;
    uint8_t group2_data;
} QrEccSpec;

static const QrEccSpec kEcc[10][4] = {
    // version 1
    { { 7, 1, 19, 0, 0 }, { 10, 1, 16, 0, 0 }, { 13, 1, 13, 0, 0 }, { 17, 1, 9, 0, 0 } },
    // version 2
    { { 10, 1, 34, 0, 0 }, { 16, 1, 28, 0, 0 }, { 22, 1, 22, 0, 0 }, { 28, 1, 16, 0, 0 } },
    // version 3
    { { 15, 1, 55, 0, 0 }, { 26, 1, 44, 0, 0 }, { 18, 2, 17, 0, 0 }, { 22, 2, 13, 0, 0 } },
    // version 4
    { { 20, 1, 80, 0, 0 }, { 18, 2, 32, 0, 0 }, { 26, 2, 24, 0, 0 }, { 16, 4, 9, 0, 0 } },
    // version 5
    { { 26, 1, 108, 0, 0 }, { 24, 2, 43, 0, 0 }, { 18, 2, 15, 2, 16 }, { 22, 2, 11, 2, 12 } },
    // version 6
    { { 18, 2, 68, 0, 0 }, { 16, 4, 27, 0, 0 }, { 24, 4, 19, 0, 0 }, { 28, 4, 15, 0, 0 } },
    // version 7
    { { 20, 2, 78, 0, 0 }, { 18, 4, 31, 0, 0 }, { 18, 2, 14, 4, 15 }, { 26, 4, 13, 1, 14 } },
    // version 8
    { { 24, 2, 97, 0, 0 }, { 22, 2, 38, 2, 39 }, { 22, 4, 18, 2, 19 }, { 26, 4, 14, 2, 15 } },
    // version 9
    { { 30, 2, 116, 0, 0 }, { 22, 3, 36, 2, 37 }, { 20, 4, 16, 4, 17 }, { 24, 4, 12, 4, 13 } },
    // version 10
    { { 18, 2, 68, 2, 69 }, { 26, 4, 43, 1, 44 }, { 24, 6, 19, 2, 20 }, { 28, 6, 15, 2, 16 } },
};

// Alignment pattern centres per version. Version 1 has none.
static const uint8_t kAlignment[10][3] = {
    { 0, 0, 0 },  { 6, 18, 0 }, { 6, 22, 0 }, { 6, 26, 0 }, { 6, 30, 0 },
    { 6, 34, 0 }, { 6, 22, 38 }, { 6, 24, 42 }, { 6, 26, 46 }, { 6, 28, 50 },
};

// ---------------------------------------------------------------------------
// Galois field GF(256), primitive polynomial 0x11D
// ---------------------------------------------------------------------------

static uint8_t g_exp[512];
static uint8_t g_log[256];
static bool g_field_ready;

static void fieldInit(void)
{
    uint16_t value = 1;

    if (g_field_ready)
        return;

    for (int i = 0; i < 255; i++) {
        g_exp[i] = (uint8_t)value;
        g_log[value] = (uint8_t)i;

        value <<= 1;

        if (value & 0x100u)
            value ^= 0x11Du;
    }

    for (int i = 255; i < 512; i++)
        g_exp[i] = g_exp[i - 255];

    g_field_ready = true;
}

static uint8_t gfMul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0)
        return 0;

    return g_exp[g_log[a] + g_log[b]];
}

// ---------------------------------------------------------------------------
// Reed-Solomon
// ---------------------------------------------------------------------------

// Builds the generator polynomial with ec_len error codewords, highest degree
// first. out[0] is always 1 and out[ec_len] is the constant term.
static void rsGenerator(int ec_len, uint8_t* out)
{
    uint8_t next[80];
    int degree = 0;

    out[0] = 1;

    for (int i = 0; i < ec_len; i++) {
        uint8_t factor[2] = { 1, g_exp[i] }; // x + alpha^i

        memset(next, 0, sizeof(next));

        for (int a = 0; a <= degree; a++) {
            for (int b = 0; b < 2; b++)
                next[a + b] ^= gfMul(out[a], factor[b]);
        }

        degree++;
        memcpy(out, next, (size_t)degree + 1);
    }
}

// Divides the message (with ec_len zero bytes appended) by the generator. The
// remainder is the error correction data.
static void rsEncode(const uint8_t* data, size_t len, int ec_len, const uint8_t* gen,
    uint8_t* out)
{
    uint8_t buffer[512];

    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, data, len);

    for (size_t i = 0; i < len; i++) {
        uint8_t coefficient = buffer[i];

        if (coefficient == 0)
            continue;

        for (int j = 1; j <= ec_len; j++)
            buffer[i + (size_t)j] ^= gfMul(gen[j], coefficient);
    }

    memcpy(out, buffer + len, (size_t)ec_len);
}

// ---------------------------------------------------------------------------
// Bit stream
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t bytes[512];
    size_t bit_count;
} QrBits;

static void bitsAppend(QrBits* bits, uint32_t value, int count)
{
    for (int i = count - 1; i >= 0; i--) {
        if ((value >> i) & 1u)
            bits->bytes[bits->bit_count / 8] |= (uint8_t)(0x80u >> (bits->bit_count % 8));

        bits->bit_count++;
    }
}

// ---------------------------------------------------------------------------
// Matrix
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t module[DGLAB_QR_MAX_SIZE][DGLAB_QR_MAX_SIZE];
    uint8_t function[DGLAB_QR_MAX_SIZE][DGLAB_QR_MAX_SIZE]; // 1 = not data
    uint8_t size;
} QrMatrix;

static void setFunction(QrMatrix* matrix, int row, int col, int dark)
{
    matrix->module[row][col] = (uint8_t)(dark ? 1 : 0);
    matrix->function[row][col] = 1;
}

static void placeFinder(QrMatrix* matrix, int row, int col)
{
    for (int r = -1; r <= 7; r++) {
        for (int c = -1; c <= 7; c++) {
            int y = row + r;
            int x = col + c;
            bool dark = false;

            if (y < 0 || x < 0 || y >= matrix->size || x >= matrix->size)
                continue;

            if (r >= 0 && r <= 6 && c >= 0 && c <= 6) {
                bool border = (r == 0 || r == 6 || c == 0 || c == 6);
                bool centre = (r >= 2 && r <= 4 && c >= 2 && c <= 4);

                dark = border || centre;
            }

            setFunction(matrix, y, x, dark);
        }
    }
}

static void placeAlignment(QrMatrix* matrix, int row, int col)
{
    for (int r = -2; r <= 2; r++) {
        for (int c = -2; c <= 2; c++) {
            bool border = (r == -2 || r == 2 || c == -2 || c == 2);
            bool centre = (r == 0 && c == 0);
            int y = row + r;
            int x = col + c;

            if (y < 0 || x < 0 || y >= matrix->size || x >= matrix->size)
                continue;

            setFunction(matrix, y, x, border || centre);
        }
    }
}

static void placePatterns(QrMatrix* matrix, int version)
{
    int size = matrix->size;

    memset(matrix->module, 0, sizeof(matrix->module));
    memset(matrix->function, 0, sizeof(matrix->function));

    placeFinder(matrix, 0, 0);
    placeFinder(matrix, 0, size - 7);
    placeFinder(matrix, size - 7, 0);

    // Timing patterns run between the finder patterns.
    for (int i = 8; i < size - 8; i++) {
        int dark = (i % 2 == 0);

        setFunction(matrix, 6, i, dark);
        setFunction(matrix, i, 6, dark);
    }

    // Alignment patterns at every combination of the centre coordinates, except
    // the three corners the finder patterns already occupy.
    {
        const uint8_t* centres = kAlignment[version - 1];
        // Version 1 has no alignment patterns at all.
        int count = 0;

        if (version > 1)
            count = (centres[2] != 0) ? 3 : 2;

        for (int i = 0; i < count; i++) {
            for (int j = 0; j < count; j++) {
                if ((i == 0 && j == 0) || (i == 0 && j == count - 1) ||
                    (i == count - 1 && j == 0))
                    continue;

                placeAlignment(matrix, centres[i], centres[j]);
            }
        }
    }

    // The dark module sits just above the bottom left format information.
    setFunction(matrix, 4 * version + 9, 8, 1);

    // Version information, two 6x3 blocks, from version 7 on.
    if (version >= 7) {
        uint32_t remainder = (uint32_t)version;
        uint32_t value;

        for (int i = 0; i < 12; i++) {
            remainder <<= 1;

            if (remainder & 0x1000u)
                remainder ^= 0x1F25u;
        }

        value = ((uint32_t)version << 12) | (remainder & 0xFFFu);

        for (int i = 0; i < 18; i++) {
            int dark = (int)((value >> i) & 1u);
            int row = i / 3;
            int col = i % 3;

            setFunction(matrix, row, size - 11 + col, dark);
            setFunction(matrix, size - 11 + col, row, dark);
        }
    }

    // Reserve the format information areas; the values are written per mask.
    for (int i = 0; i < 9; i++) {
        if (i != 6) {
            setFunction(matrix, 8, i, 0);
            setFunction(matrix, i, 8, 0);
        }
    }

    // Second copy: eight modules along row 8, seven down column 8. The row
    // below this strip is the dark module, so it must not be reserved here.
    for (int i = 0; i < 8; i++)
        setFunction(matrix, 8, size - 1 - i, 0);

    for (int i = 0; i < 7; i++)
        setFunction(matrix, size - 1 - i, 8, 0);
}

static void placeFormat(QrMatrix* matrix, uint8_t ecc_bits, int mask)
{
    int size = matrix->size;
    uint32_t value = ((uint32_t)ecc_bits << 3) | (uint32_t)mask;
    uint32_t remainder = value;
    uint32_t encoded;

    for (int i = 0; i < 10; i++) {
        remainder <<= 1;

        if (remainder & 0x400u)
            remainder ^= 0x537u;
    }

    encoded = ((value << 10) | (remainder & 0x3FFu)) ^ 0x5412u;

    for (int i = 0; i < 15; i++) {
        int dark = (int)((encoded >> i) & 1u);
        int row;
        int col;

        // First copy: down the column beside the top left finder, then along
        // its row.
        if (i < 6) {
            row = i;
            col = 8;
        } else if (i < 8) {
            row = i + 1;
            col = 8;
        } else if (i == 8) {
            row = 8;
            col = 7;
        } else {
            row = 8;
            col = 14 - i;
        }

        setFunction(matrix, row, col, dark);

        // Second copy, split between the top right and bottom left finders.
        if (i < 8) {
            row = 8;
            col = size - 1 - i;
        } else {
            row = size - 15 + i;
            col = 8;
        }

        setFunction(matrix, row, col, dark);
    }
}

// ---------------------------------------------------------------------------
// Data placement and masking
// ---------------------------------------------------------------------------

static bool maskApplies(int mask, int row, int col)
{
    switch (mask) {
        case 0: return (row + col) % 2 == 0;
        case 1: return row % 2 == 0;
        case 2: return col % 3 == 0;
        case 3: return (row + col) % 3 == 0;
        case 4: return (row / 2 + col / 3) % 2 == 0;
        case 5: return ((row * col) % 2) + ((row * col) % 3) == 0;
        case 6: return (((row * col) % 2) + ((row * col) % 3)) % 2 == 0;
        case 7: return (((row + col) % 2) + ((row * col) % 3)) % 2 == 0;
        default: return false;
    }
}

// Places the interleaved codeword bits in two module wide columns, right to
// left, alternating upward and downward. Modules past the last bit stay light,
// which is exactly what the remainder bits are.
static void placeData(QrMatrix* matrix, const uint8_t* codewords, size_t count)
{
    size_t bit = 0;
    size_t total_bits = count * 8u;
    bool upward = true;
    int size = matrix->size;

    for (int col = size - 1; col > 0; col -= 2) {
        if (col == 6)
            col--; // the vertical timing pattern column is skipped

        for (int i = 0; i < size; i++) {
            int row = upward ? (size - 1 - i) : i;

            for (int c = 0; c < 2; c++) {
                int x = col - c;
                int dark = 0;

                if (matrix->function[row][x])
                    continue;

                if (bit < total_bits)
                    dark = (codewords[bit / 8] >> (7 - (bit % 8))) & 1;

                matrix->module[row][x] = (uint8_t)dark;
                bit++;
            }
        }

        upward = !upward;
    }
}

static void applyMask(QrMatrix* matrix, int mask)
{
    for (int row = 0; row < matrix->size; row++) {
        for (int col = 0; col < matrix->size; col++) {
            if (matrix->function[row][col])
                continue;

            if (maskApplies(mask, row, col))
                matrix->module[row][col] ^= 1u;
        }
    }
}

// ---------------------------------------------------------------------------
// Mask selection: the four penalty rules of the standard
// ---------------------------------------------------------------------------

static int penaltyRuns(const QrMatrix* matrix)
{
    int penalty = 0;

    for (int line = 0; line < matrix->size; line++) {
        for (int direction = 0; direction < 2; direction++) {
            int run = 1;
            int previous = direction ? matrix->module[0][line] : matrix->module[line][0];

            for (int i = 1; i < matrix->size; i++) {
                int value = direction ? matrix->module[i][line] : matrix->module[line][i];

                if (value == previous) {
                    run++;
                } else {
                    if (run >= 5)
                        penalty += 3 + (run - 5);

                    previous = value;
                    run = 1;
                }
            }

            if (run >= 5)
                penalty += 3 + (run - 5);
        }
    }

    return penalty;
}

static int penaltyBlocks(const QrMatrix* matrix)
{
    int penalty = 0;

    for (int row = 0; row + 1 < matrix->size; row++) {
        for (int col = 0; col + 1 < matrix->size; col++) {
            int value = matrix->module[row][col];

            if (matrix->module[row][col + 1] == value && matrix->module[row + 1][col] == value &&
                matrix->module[row + 1][col + 1] == value)
                penalty += 3;
        }
    }

    return penalty;
}

static int penaltyFinderLike(const QrMatrix* matrix)
{
    // 1011101 with four light modules before or after it.
    static const uint8_t kPatterns[2][11] = {
        { 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0 },
        { 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1 },
    };
    int penalty = 0;

    for (int line = 0; line < matrix->size; line++) {
        for (int direction = 0; direction < 2; direction++) {
            for (int start = 0; start + 11 <= matrix->size; start++) {
                for (int pattern = 0; pattern < 2; pattern++) {
                    bool matches = true;

                    for (int i = 0; i < 11 && matches; i++) {
                        int value = direction ? matrix->module[start + i][line]
                                              : matrix->module[line][start + i];

                        if (value != kPatterns[pattern][i])
                            matches = false;
                    }

                    if (matches)
                        penalty += 40;
                }
            }
        }
    }

    return penalty;
}

static int penaltyBalance(const QrMatrix* matrix)
{
    int dark = 0;
    int total = matrix->size * matrix->size;

    for (int row = 0; row < matrix->size; row++) {
        for (int col = 0; col < matrix->size; col++)
            dark += matrix->module[row][col];
    }

    {
        int percent = (dark * 100) / total;
        int difference = (percent > 50) ? (percent - 50) : (50 - percent);

        return 10 * (difference / 5);
    }
}

static int penaltyScore(const QrMatrix* matrix)
{
    return penaltyRuns(matrix) + penaltyBlocks(matrix) + penaltyFinderLike(matrix) +
           penaltyBalance(matrix);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static size_t dataCapacity(uint8_t version, DglabQrEcc ecc)
{
    const QrEccSpec* spec = &kEcc[version - 1][ecc];

    return (size_t)spec->group1_blocks * spec->group1_data +
           (size_t)spec->group2_blocks * spec->group2_data;
}

static int countBits(uint8_t version)
{
    return (version <= 9) ? 8 : 16;
}

uint8_t dglabQrFindVersion(size_t size, DglabQrEcc ecc)
{
    for (uint8_t version = 1; version <= 10; version++) {
        size_t needed_bits = 4u + (size_t)countBits(version) + size * 8u;
        size_t needed_bytes = (needed_bits + 7u) / 8u;

        if (needed_bytes <= dataCapacity(version, ecc))
            return version;
    }

    return 0;
}

bool dglabQrEncode(DglabQrCode* out, const void* data, size_t size, DglabQrEcc ecc)
{
    // Two error correction levels have a two bit indicator: L = 01, M = 00,
    // Q = 11, H = 10.
    static const uint8_t kEccBits[4] = { 1, 0, 3, 2 };
    QrMatrix matrix;
    QrMatrix best;
    QrBits bits;
    const QrEccSpec* spec;
    uint8_t data_codewords[512];
    uint8_t ec_codewords[512];
    uint8_t generator[80];
    uint8_t version;
    int best_mask = -1;
    int best_score = 0;
    size_t data_len;
    size_t ec_len;
    int block_count;
    int max_block_data;

    memset(&best, 0, sizeof(best));

    if (!out || (!data && size != 0))
        return false;

    if (size > 512)
        return false;

    version = dglabQrFindVersion(size, ecc);

    if (version == 0)
        return false;

    fieldInit();

    spec = &kEcc[version - 1][ecc];
    data_len = dataCapacity(version, ecc);
    ec_len = spec->ec_codewords;
    block_count = spec->group1_blocks + spec->group2_blocks;
    max_block_data = (spec->group2_data > spec->group1_data) ? spec->group2_data
                                                            : spec->group1_data;

    // Bit stream: mode, character count, payload, terminator and padding.
    memset(&bits, 0, sizeof(bits));
    bitsAppend(&bits, 0x4, 4); // byte mode
    bitsAppend(&bits, (uint32_t)size, countBits(version));

    for (size_t i = 0; i < size; i++)
        bitsAppend(&bits, ((const uint8_t*)data)[i], 8);

    if (bits.bit_count + 4 <= data_len * 8u)
        bitsAppend(&bits, 0, 4);

    while (bits.bit_count % 8 != 0)
        bitsAppend(&bits, 0, 1);

    {
        size_t written = bits.bit_count / 8;
        uint8_t pad = 0xEC;

        while (written < data_len) {
            bits.bytes[written++] = pad;
            pad = (uint8_t)((pad == 0xEC) ? 0x11 : 0xEC);
        }
    }

    // Split the data into blocks and add each block's error correction data.
    {
        size_t offset = 0;
        size_t block_start = 0;
        size_t ec_total = 0;
        int block_index = 0;

        rsGenerator((int)ec_len, generator);

        for (int group = 0; group < 2; group++) {
            int blocks = (group == 0) ? spec->group1_blocks : spec->group2_blocks;
            int block_data = (group == 0) ? spec->group1_data : spec->group2_data;

            for (int b = 0; b < blocks; b++) {
                memcpy(data_codewords + offset, bits.bytes + block_start, (size_t)block_data);
                rsEncode(data_codewords + offset, (size_t)block_data, (int)ec_len, generator,
                    ec_codewords + ec_total);

                offset += (size_t)block_data;
                block_start += (size_t)block_data;
                ec_total += ec_len;
                block_index++;
            }
        }

        (void)block_index;
    }

    // Interleave: data codewords column by column across the blocks, then the
    // error correction codewords the same way.
    {
        uint8_t interleaved[512];
        size_t written = 0;

        for (int i = 0; i < max_block_data; i++) {
            size_t block_offset = 0;

            for (int group = 0; group < 2; group++) {
                int blocks = (group == 0) ? spec->group1_blocks : spec->group2_blocks;
                int block_data = (group == 0) ? spec->group1_data : spec->group2_data;

                for (int b = 0; b < blocks; b++) {
                    if (i < block_data)
                        interleaved[written++] = data_codewords[block_offset + (size_t)i];

                    block_offset += (size_t)block_data;
                }
            }
        }

        for (size_t i = 0; i < ec_len; i++) {
            for (int b = 0; b < block_count; b++)
                interleaved[written++] = ec_codewords[(size_t)b * ec_len + i];
        }

        memcpy(data_codewords, interleaved, written);
        data_len = written;
    }

    // Build the symbol for every mask and keep the lowest penalty.
    for (int mask = 0; mask < 8; mask++) {
        int score;

        memset(&matrix, 0, sizeof(matrix));
        matrix.size = (uint8_t)(17 + 4 * version);

        placePatterns(&matrix, version);
        placeData(&matrix, data_codewords, data_len);
        applyMask(&matrix, mask);
        placeFormat(&matrix, kEccBits[ecc], mask);

        score = penaltyScore(&matrix);

        if (best_mask < 0 || score < best_score) {
            best_mask = mask;
            best_score = score;
            best = matrix;
        }
    }

    out->size = best.size;
    out->version = version;
    out->mask = (uint8_t)best_mask;
    out->ecc = ecc;

    for (int row = 0; row < best.size; row++) {
        for (int col = 0; col < best.size; col++)
            out->modules[row][col] = best.module[row][col];
    }

    return true;
}

bool dglabQrEncodeString(DglabQrCode* out, const char* text, DglabQrEcc ecc)
{
    if (!text)
        return false;

    return dglabQrEncode(out, text, strlen(text), ecc);
}
