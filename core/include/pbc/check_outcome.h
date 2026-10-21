#pragma once
// "Check the server" for an item whose outcome is unknown (specs/002-manage-server-files, research R-7): read-only, it only reports what it finds
// and never decides for the user. One or two PROPFINDs of Depth 0. Qt-free.

#include <string_view>

#include "pbc/dav_common.h"
#include "pbc/queue_types.h"

namespace pbc {

enum class Found {
    Arrived,        // what the item should have made is there (the file with its size and, when the server reports it, its checksum; the folder; the moved entry)
    NotThere,       // nothing of it is on the server
    SomethingElse,  // a different file or entry with the name is there
    Unclear,        // the check itself failed (no connection, an answer that could not be used)
};

Found check_outcome(DavSession& session, std::string_view user_id, const QueueItem& item);

}  // namespace pbc
