#pragma once
// Small text helpers shared by the bridges. Names that come from a server are passed through display_safe() before they reach QML.

#include <QString>

#include <string>

// "812 B", "4.2 KB", "1.5 MB", "12.3 GB" (binary units, one decimal from KB up). Negative values give "".
QString human_size(qint64 bytes);

// A name from the server for display: control characters (and the bidirectional override characters that could reorder the line) are
// removed, invalid UTF-8 is replaced, at most `max_chars` characters are kept ("..." marks a cut).
QString display_safe(const std::string& name, int max_chars = 200);

// A text of the interface that a bridge produces itself (the pages use qsTr). It is looked up in the same German table (qml/strings_de.tsv) through
// the installed translator; scripts/extract-strings.py collects these literals as well, so the table has no gaps and no stale lines.
QString bridge_tr(const char* english_source);
