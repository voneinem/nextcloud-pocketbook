#pragma once
// A small QR code encoder (specs/001-nextcloud-files-client, research R-5): byte mode, error correction level L, versions 1-12 (up to
// 367 bytes), mask chosen by the standard penalty rule. It draws the link of the browser login so that nobody has to type it. Original
// code written from ISO/IEC 18004; no third-party code. Qt-free.

#include <cstdint>
#include <string_view>
#include <vector>

#include "pbc/types.h"

namespace pbc {

struct QrMatrix {
    int size = 0;                       // modules per side: 17 + 4 * version
    std::vector<uint8_t> modules;       // size * size, row by row, 1 = dark
    bool at(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && modules[static_cast<size_t>(y) * size + x] != 0; }
};

// Error key "qr_too_long" for empty input and for input above 367 bytes. The text is encoded as its bytes (UTF-8 stays UTF-8).
Result<QrMatrix> qr_encode(std::string_view text);

}  // namespace pbc
