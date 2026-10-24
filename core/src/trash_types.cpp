#include "pbc/trash_types.h"

// The types of the trash and versions screens are plain data; this file exists so that the module has one translation unit and the limits
// are checked at compile time.
namespace pbc {

static_assert(kMaxTrashRows == 1000, "the stored trash list holds at most 1,000 rows (data-model.md)");
static_assert(kMaxVersionRows == 200, "a version list keeps at most 200 rows (data-model.md)");
static_assert(kMaxTrashBody == 4u * 1024u * 1024u && kMaxVersionBody == 1024u * 1024u, "answer limits of research R-3");
static_assert(kMaxLocationBytes == 1024 && kMaxSegmentBytes == 255, "name and location limits of research R-3");

}  // namespace pbc
