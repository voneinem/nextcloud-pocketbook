#include "pbc/qr_code.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace pbc {

namespace {

// Per version (error correction level L): error correction codewords per block, and the blocks as {count, data codewords} for the
// two groups (ISO/IEC 18004 table 9).
struct VersionInfo {
    int ec_per_block;
    int g1_blocks, g1_data;
    int g2_blocks, g2_data;
};

constexpr VersionInfo kVersions[13] = {
    {0, 0, 0, 0, 0},      // 0 unused
    {7, 1, 19, 0, 0},     {10, 1, 34, 0, 0},   {15, 1, 55, 0, 0},   {20, 1, 80, 0, 0},    {26, 1, 108, 0, 0},
    {18, 2, 68, 0, 0},    {20, 2, 78, 0, 0},   {24, 2, 97, 0, 0},   {30, 2, 116, 0, 0},   {18, 2, 68, 2, 69},
    {20, 4, 81, 0, 0},    {24, 2, 92, 2, 93},
};

constexpr int kMaxVersion = 12;

int data_codewords(int v) { return kVersions[v].g1_blocks * kVersions[v].g1_data + kVersions[v].g2_blocks * kVersions[v].g2_data; }
// Bytes that fit in byte mode: 4 bits mode, 8 or 16 bits count, the data.
int byte_capacity(int v) { return (data_codewords(v) * 8 - 4 - (v >= 10 ? 16 : 8)) / 8; }

std::vector<int> alignment_centers(int v) {
    static const int table[13][4] = {{0}, {0}, {6, 18, 0, 0}, {6, 22, 0, 0}, {6, 26, 0, 0}, {6, 30, 0, 0}, {6, 34, 0, 0},
                                     {6, 22, 38, 0}, {6, 24, 42, 0}, {6, 26, 46, 0}, {6, 28, 50, 0}, {6, 30, 54, 0}, {6, 32, 58, 0}};
    std::vector<int> out;
    if (v < 2) return out;
    for (int i = 0; i < 4 && table[v][i] != 0; ++i) out.push_back(table[v][i]);
    return out;
}

// ---- Reed-Solomon over GF(256), primitive polynomial 0x11D ----
struct Gf {
    std::array<uint8_t, 256> exp{};
    std::array<int, 256> log{};
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[static_cast<size_t>(i)] = static_cast<uint8_t>(x);
            log[static_cast<size_t>(x)] = i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        exp[255] = exp[0];
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a == 0 || b == 0) ? 0 : exp[static_cast<size_t>((log[a] + log[b]) % 255)]; }
};

const Gf& gf() {
    static const Gf g;
    return g;
}

std::vector<uint8_t> rs_generator(int degree) {
    std::vector<uint8_t> g(static_cast<size_t>(degree), 0);
    g[static_cast<size_t>(degree - 1)] = 1;   // the polynomial is stored without its leading 1, highest power first
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (int j = 0; j < degree; ++j) {
            g[static_cast<size_t>(j)] = gf().mul(g[static_cast<size_t>(j)], root);
            if (j + 1 < degree) g[static_cast<size_t>(j)] ^= g[static_cast<size_t>(j + 1)];
        }
        root = gf().mul(root, 2);
    }
    return g;
}

std::vector<uint8_t> rs_remainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& generator) {
    std::vector<uint8_t> result(generator.size(), 0);
    for (uint8_t b : data) {
        const uint8_t factor = b ^ result[0];
        result.erase(result.begin());
        result.push_back(0);
        for (size_t i = 0; i < generator.size(); ++i) result[i] ^= gf().mul(generator[i], factor);
    }
    return result;
}

// ---- the symbol ----
class Symbol {
public:
    explicit Symbol(int version) : version_(version), size_(17 + 4 * version), modules_(static_cast<size_t>(size_ * size_), 0), function_(static_cast<size_t>(size_ * size_), 0) {
        draw_function_patterns();
    }

    void draw_codewords(const std::vector<uint8_t>& data) {
        size_t i = 0;
        const size_t total_bits = data.size() * 8;
        for (int right = size_ - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int vert = 0; vert < size_; ++vert) {
                for (int j = 0; j < 2; ++j) {
                    const int x = right - j;
                    const bool upward = ((right + 1) & 2) == 0;
                    const int y = upward ? size_ - 1 - vert : vert;
                    if (!function_[idx(x, y)] && i < total_bits) {
                        modules_[idx(x, y)] = (data[i >> 3] >> (7 - (i & 7))) & 1;
                        ++i;
                    }
                }
            }
        }
    }

    // Picks the mask with the lowest penalty (the first one on a tie), applies it and draws the format bits.
    void finish() {
        int best = 0;
        long best_penalty = -1;
        for (int mask = 0; mask < 8; ++mask) {
            apply_mask(mask);
            draw_format_bits(mask);
            const long p = penalty();
            if (best_penalty < 0 || p < best_penalty) {
                best = mask;
                best_penalty = p;
            }
            apply_mask(mask);   // undo (the mask is its own inverse)
        }
        apply_mask(best);
        draw_format_bits(best);
    }

    QrMatrix matrix() const {
        QrMatrix m;
        m.size = size_;
        m.modules = modules_;
        return m;
    }

private:
    size_t idx(int x, int y) const { return static_cast<size_t>(y) * static_cast<size_t>(size_) + static_cast<size_t>(x); }
    bool dark(int x, int y) const { return modules_[idx(x, y)] != 0; }
    void set_function(int x, int y, bool d) {
        modules_[idx(x, y)] = d ? 1 : 0;
        function_[idx(x, y)] = 1;
    }

    void draw_function_patterns() {
        for (int i = 0; i < size_; ++i) {
            set_function(6, i, i % 2 == 0);
            set_function(i, 6, i % 2 == 0);
        }
        draw_finder(3, 3);
        draw_finder(size_ - 4, 3);
        draw_finder(3, size_ - 4);
        const std::vector<int> centers = alignment_centers(version_);
        const size_t n = centers.size();
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < n; ++j) {
                if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;   // they would overlap a finder
                draw_alignment(centers[i], centers[j]);
            }
        draw_format_bits(0);   // reserves the area; the real bits follow after the mask is chosen
        draw_version();
    }
    void draw_finder(int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                const int dist = std::max(std::abs(dx), std::abs(dy));
                const int x = cx + dx, y = cy + dy;
                if (x >= 0 && x < size_ && y >= 0 && y < size_) set_function(x, y, dist != 2 && dist != 4);
            }
    }
    void draw_alignment(int cx, int cy) {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) set_function(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
    }
    static bool bit(unsigned v, int i) { return ((v >> i) & 1) != 0; }
    void draw_format_bits(int mask) {
        const unsigned data = (1u << 3) | static_cast<unsigned>(mask);   // level L = 01
        unsigned rem = data;
        for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        const unsigned bits = ((data << 10) | rem) ^ 0x5412;
        for (int i = 0; i <= 5; ++i) set_function(8, i, bit(bits, i));
        set_function(8, 7, bit(bits, 6));
        set_function(8, 8, bit(bits, 7));
        set_function(7, 8, bit(bits, 8));
        for (int i = 9; i < 15; ++i) set_function(14 - i, 8, bit(bits, i));
        for (int i = 0; i < 8; ++i) set_function(size_ - 1 - i, 8, bit(bits, i));
        for (int i = 8; i < 15; ++i) set_function(8, size_ - 15 + i, bit(bits, i));
        set_function(8, size_ - 8, true);   // the dark module
    }
    void draw_version() {
        if (version_ < 7) return;
        unsigned rem = static_cast<unsigned>(version_);
        for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        const unsigned bits = (static_cast<unsigned>(version_) << 12) | rem;
        for (int i = 0; i < 18; ++i) {
            const bool b = bit(bits, i);
            const int a = size_ - 11 + i % 3;
            const int c = i / 3;
            set_function(a, c, b);
            set_function(c, a, b);
        }
    }

    static bool masked(int mask, int x, int y) {
        switch (mask) {
            case 0: return (x + y) % 2 == 0;
            case 1: return y % 2 == 0;
            case 2: return x % 3 == 0;
            case 3: return (x + y) % 3 == 0;
            case 4: return (x / 3 + y / 2) % 2 == 0;
            case 5: return x * y % 2 + x * y % 3 == 0;
            case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
            default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
        }
    }
    void apply_mask(int mask) {
        for (int y = 0; y < size_; ++y)
            for (int x = 0; x < size_; ++x)
                if (!function_[idx(x, y)] && masked(mask, x, y)) modules_[idx(x, y)] ^= 1;
    }

    // The penalty score of ISO/IEC 18004 section 7.8.3: runs of five or more, 2x2 blocks, finder-like patterns (1011101 with four light
    // modules on one side; the border of the symbol does not count as light), and the balance of dark and light modules.
    long penalty() const {
        long score = 0;
        long dark_count = 0;
        for (int y = 0; y < size_; ++y)
            for (int x = 0; x < size_; ++x) {
                dark_count += dark(x, y) ? 1 : 0;
                if (x + 1 < size_ && y + 1 < size_ && dark(x, y) == dark(x + 1, y) && dark(x, y) == dark(x, y + 1) && dark(x, y) == dark(x + 1, y + 1)) score += 3;
            }
        for (int pass = 0; pass < 2; ++pass) {   // rows, then columns
            for (int a = 0; a < size_; ++a) {
                auto d = [&](int k) { return pass == 0 ? dark(k, a) : dark(a, k); };
                int run = 1;
                for (int k = 1; k < size_; ++k) {
                    if (d(k) == d(k - 1)) {
                        ++run;
                    } else {
                        if (run >= 5) score += 3 + (run - 5);
                        run = 1;
                    }
                }
                if (run >= 5) score += 3 + (run - 5);
                for (int c = 0; c + 7 <= size_; ++c) {
                    if (!(d(c) && !d(c + 1) && d(c + 2) && d(c + 3) && d(c + 4) && !d(c + 5) && d(c + 6))) continue;
                    bool before = c >= 4, after = c + 7 + 4 <= size_;
                    for (int k = 1; before && k <= 4; ++k) before = !d(c - k);
                    for (int k = 0; after && k < 4; ++k) after = !d(c + 7 + k);
                    if (before) score += 40;
                    if (after) score += 40;
                }
            }
        }
        const long total = static_cast<long>(size_) * size_;
        const long k = std::labs(dark_count * 20 - total * 10) / total;   // steps of 5 % away from an even balance
        score += k * 10;
        return score;
    }

    int version_;
    int size_;
    std::vector<uint8_t> modules_;
    std::vector<uint8_t> function_;
};

}  // namespace

Result<QrMatrix> qr_encode(std::string_view text) {
    Result<QrMatrix> r;
    int version = 0;
    for (int v = 1; v <= kMaxVersion; ++v)
        if (static_cast<int>(text.size()) <= byte_capacity(v)) {
            version = v;
            break;
        }
    if (text.empty() || version == 0) {
        r.error = make_error(ErrorClass::Unsupported, "qr_too_long", "the text does not fit in a QR code");
        return r;
    }
    // the bit stream: mode 0100, the length, the bytes, a terminator, padding to the data capacity
    std::vector<uint8_t> bits;
    auto put = [&](unsigned value, int count) {
        for (int i = count - 1; i >= 0; --i) bits.push_back(static_cast<uint8_t>((value >> i) & 1));
    };
    put(4, 4);
    put(static_cast<unsigned>(text.size()), version >= 10 ? 16 : 8);
    for (unsigned char c : text) put(c, 8);
    const size_t capacity_bits = static_cast<size_t>(data_codewords(version)) * 8;
    for (int i = 0; i < 4 && bits.size() < capacity_bits; ++i) bits.push_back(0);
    while (bits.size() % 8 != 0) bits.push_back(0);
    std::vector<uint8_t> data;
    for (size_t i = 0; i < bits.size(); i += 8) {
        uint8_t b = 0;
        for (int k = 0; k < 8; ++k) b = static_cast<uint8_t>((b << 1) | bits[i + static_cast<size_t>(k)]);
        data.push_back(b);
    }
    for (uint8_t pad = 0xEC; data.size() < static_cast<size_t>(data_codewords(version)); pad ^= 0xEC ^ 0x11) data.push_back(pad);

    // blocks, error correction, interleaving
    const VersionInfo& vi = kVersions[version];
    const int blocks = vi.g1_blocks + vi.g2_blocks;
    const std::vector<uint8_t> generator = rs_generator(vi.ec_per_block);
    std::vector<std::vector<uint8_t>> data_blocks, ec_blocks;
    size_t offset = 0;
    for (int b = 0; b < blocks; ++b) {
        const int len = b < vi.g1_blocks ? vi.g1_data : vi.g2_data;
        data_blocks.emplace_back(data.begin() + static_cast<long>(offset), data.begin() + static_cast<long>(offset) + len);
        offset += static_cast<size_t>(len);
        ec_blocks.push_back(rs_remainder(data_blocks.back(), generator));
    }
    std::vector<uint8_t> stream;
    const size_t longest = static_cast<size_t>(vi.g2_blocks > 0 ? vi.g2_data : vi.g1_data);
    for (size_t i = 0; i < longest; ++i)
        for (const auto& blk : data_blocks)
            if (i < blk.size()) stream.push_back(blk[i]);
    for (int i = 0; i < vi.ec_per_block; ++i)
        for (const auto& blk : ec_blocks) stream.push_back(blk[static_cast<size_t>(i)]);

    Symbol symbol(version);
    symbol.draw_codewords(stream);
    symbol.finish();
    r.value = symbol.matrix();
    return r;
}

}  // namespace pbc
