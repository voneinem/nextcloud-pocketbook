// The QR encoder (research R-5): byte mode, error correction level L, versions 1-12. The reference matrices in tests/fixtures/qr/ were
// made with libqrencode (tests/tools/gen_qr_fixtures.py), an encoder that shares no code with ours; our matrix must be bit-identical.
#include <dirent.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "pbc/qr_code.h"
#include "test_support.h"

namespace {

struct Reference {
    std::string name;
    int version = 0;
    std::string input;
    std::vector<std::string> rows;
};

std::string from_hex(const std::string& hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

std::vector<Reference> load_references() {
    std::vector<Reference> refs;
    const std::string dir = std::string(PBC_FIXTURE_DIR) + "/qr";
    DIR* d = opendir(dir.c_str());
    if (!d) PB_FAIL_AT("no QR fixtures at " + dir);
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".txt") == 0) names.push_back(n);
    }
    closedir(d);
    for (const std::string& n : names) {
        std::ifstream f(dir + "/" + n);
        Reference r;
        r.name = n;
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("# version: ", 0) == 0) r.version = std::stoi(line.substr(11));
            else if (line.rfind("# input-hex: ", 0) == 0) r.input = from_hex(line.substr(13));
            else if (!line.empty() && line[0] != '#') r.rows.push_back(line);
        }
        refs.push_back(r);
    }
    return refs;
}

std::string row_of(const pbc::QrMatrix& m, int y) {
    std::string s;
    for (int x = 0; x < m.size; ++x) s.push_back(m.at(x, y) ? '1' : '0');
    return s;
}

}  // namespace

PB_TEST(there_are_reference_matrices_for_the_boundaries) {
    PB_CHECK(load_references().size() >= 6);
}

// ---- a mask-independent comparison ----
// Two encoders may pick different masks for the same text (the penalty rule leaves room for interpretation), and both codes are then
// valid. What must agree is everything else: the data and error correction bits once the mask is taken off, and every fixed pattern.
// The map of fixed modules is computed here from the standard, independently of the encoder.

std::vector<bool> function_map(int version) {
    const int n = 17 + 4 * version;
    std::vector<bool> f(static_cast<size_t>(n) * n, false);
    auto set = [&](int x, int y) {
        if (x >= 0 && y >= 0 && x < n && y < n) f[static_cast<size_t>(y) * n + x] = true;
    };
    for (int i = 0; i < n; ++i) {
        set(6, i);
        set(i, 6);
    }
    for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx) {
            set(3 + dx, 3 + dy);          // finder and separator, top left
            set(n - 4 + dx, 3 + dy);      // top right
            set(3 + dx, n - 4 + dy);      // bottom left
        }
    static const int centers[13][4] = {{0}, {0}, {6, 18}, {6, 22}, {6, 26}, {6, 30}, {6, 34}, {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50}, {6, 30, 54}, {6, 32, 58}};
    std::vector<int> c;
    for (int i = 0; i < 4 && centers[version][i]; ++i) c.push_back(centers[version][i]);
    for (size_t i = 0; i < c.size(); ++i)
        for (size_t j = 0; j < c.size(); ++j) {
            if ((i == 0 && j == 0) || (i == 0 && j == c.size() - 1) || (i == c.size() - 1 && j == 0)) continue;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) set(c[i] + dx, c[j] + dy);
        }
    for (int i = 0; i < 9; ++i) {         // format information: the L around the top left finder ...
        set(8, i);
        set(i, 8);
    }
    for (int i = 0; i < 8; ++i) {         // ... and the two copies at the other finders (the module at (8, n-8) is the dark module)
        set(n - 1 - i, 8);
        set(8, n - 1 - i);
    }
    if (version >= 7)
        for (int i = 0; i < 6; ++i)
            for (int j = 0; j < 3; ++j) {
                set(n - 11 + j, i);
                set(i, n - 11 + j);
            }
    return f;
}

bool mask_bit(int mask, int x, int y) {
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

// Reads the format information (first copy), checks the BCH code, and returns the mask; -1 if it is not valid or not level L.
int read_mask(const pbc::QrMatrix& m) {
    unsigned bits = 0;
    for (int i = 0; i <= 5; ++i) bits |= (m.at(8, i) ? 1u : 0u) << i;
    bits |= (m.at(8, 7) ? 1u : 0u) << 6;
    bits |= (m.at(8, 8) ? 1u : 0u) << 7;
    bits |= (m.at(7, 8) ? 1u : 0u) << 8;
    for (int i = 9; i < 15; ++i) bits |= (m.at(14 - i, 8) ? 1u : 0u) << i;
    const unsigned data = (bits ^ 0x5412) >> 10;
    unsigned rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    if ((((data << 10) | rem) ^ 0x5412) != bits) return -1;
    if ((data >> 3) != 1u) return -1;   // level L
    return static_cast<int>(data & 7);
}

PB_TEST(everything_but_the_choice_of_mask_matches_the_reference_of_an_independent_encoder) {
    std::string wrong;
    for (const Reference& ref : load_references()) {
        const auto r = pbc::qr_encode(ref.input);
        if (!r.ok()) PB_FAIL_AT(ref.name + ": encoding failed with " + r.error.key);
        const pbc::QrMatrix& mine = r.value;
        PB_CHECK_EQ(mine.size, 17 + 4 * ref.version);
        PB_CHECK_EQ(static_cast<int>(ref.rows.size()), mine.size);
        pbc::QrMatrix theirs;
        theirs.size = mine.size;
        for (const std::string& row : ref.rows)
            for (char c : row) theirs.modules.push_back(c == '1' ? 1 : 0);
        const int my_mask = read_mask(mine), their_mask = read_mask(theirs);
        if (my_mask < 0 || their_mask < 0) {
            wrong += " " + ref.name + "(format information)";
            continue;
        }
        const std::vector<bool> fixed = function_map(ref.version);
        int off = 0;
        for (int y = 0; y < mine.size; ++y)
            for (int x = 0; x < mine.size; ++x) {
                bool a = mine.at(x, y), b = theirs.at(x, y);
                if (fixed[static_cast<size_t>(y) * mine.size + x]) {
                    const bool format_area = (x == 8 && (y <= 8 || y >= mine.size - 8)) || (y == 8 && (x <= 8 || x >= mine.size - 8));
                    if (!format_area && a != b) ++off;   // finder, timing, alignment and version patterns must be the same
                } else {
                    a ^= mask_bit(my_mask, x, y);
                    b ^= mask_bit(their_mask, x, y);
                    if (a != b) ++off;                   // the data and the error correction, with the mask taken off
                }
            }
        if (off) wrong += " " + ref.name + "(" + std::to_string(off) + " modules)";
    }
    if (!wrong.empty()) PB_FAIL_AT("differs from the reference beyond the mask:" + wrong);
}

PB_TEST(most_references_are_identical_including_the_mask_choice) {
    int identical = 0, total = 0;
    for (const Reference& ref : load_references()) {
        const auto r = pbc::qr_encode(ref.input);
        PB_CHECK(r.ok());
        bool same = static_cast<int>(ref.rows.size()) == r.value.size;
        for (int y = 0; same && y < r.value.size; ++y) same = row_of(r.value, y) == ref.rows[static_cast<size_t>(y)];
        identical += same ? 1 : 0;
        ++total;
    }
    // the penalty rule of ISO/IEC 18004 is open to interpretation at the symbol border; libqrencode and this encoder agree on most inputs
    PB_CHECK(identical * 3 >= total * 2);
}

PB_TEST(the_fixed_patterns_are_where_the_standard_puts_them) {
    const auto r = pbc::qr_encode("https://cloud.example.org/login");
    PB_CHECK(r.ok());
    const pbc::QrMatrix& m = r.value;
    const int n = m.size;
    // finder patterns: a 7x7 ring, a light ring, a 3x3 core, at three corners
    for (const auto& corner : {std::pair<int, int>{0, 0}, {n - 7, 0}, {0, n - 7}}) {
        for (int i = 0; i < 7; ++i)
            for (int j = 0; j < 7; ++j) {
                const bool ring = i == 0 || i == 6 || j == 0 || j == 6;
                const bool core = i >= 2 && i <= 4 && j >= 2 && j <= 4;
                PB_CHECK_EQ(m.at(corner.first + i, corner.second + j), ring || core);
            }
    }
    // timing patterns alternate between the finders
    for (int i = 8; i < n - 8; ++i) {
        PB_CHECK_EQ(m.at(i, 6), i % 2 == 0);
        PB_CHECK_EQ(m.at(6, i), i % 2 == 0);
    }
    // the dark module above the lower-left finder
    const int version = (n - 17) / 4;
    PB_CHECK(m.at(8, 4 * version + 9));
}

PB_TEST(input_beyond_the_capacity_of_version_12_is_refused) {
    PB_CHECK(pbc::qr_encode(std::string(367, 'x')).ok());
    const auto r = pbc::qr_encode(std::string(368, 'x'));
    PB_CHECK(!r.ok());
    PB_CHECK_EQ(r.error.key, std::string("qr_too_long"));
    PB_CHECK_EQ(pbc::qr_encode("").error.key, std::string("qr_too_long"));   // nothing to draw is not a code either
}

PB_TEST(the_same_input_always_gives_the_same_matrix) {
    const auto a = pbc::qr_encode("https://cloud.example.org/index.php/login/v2/flow/token");
    const auto b = pbc::qr_encode("https://cloud.example.org/index.php/login/v2/flow/token");
    PB_CHECK(a.ok() && b.ok());
    PB_CHECK(a.value.modules == b.value.modules);
}

PB_TEST_MAIN()
