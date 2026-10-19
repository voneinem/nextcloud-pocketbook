#include "pbc/version.h"

#ifndef PBC_VERSION
#define PBC_VERSION "0.0.0"
#endif

namespace pbc {

const char* version() { return PBC_VERSION; }

}  // namespace pbc
