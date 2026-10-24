// The argument list of a hand-off to the mail app (specs/005-mail-link, contracts/handoff-call.md): what is handed over and in which form. Qt-free.
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "pbc/mail_handoff.h"
#include "pbc/utf8.h"
#include "test_support.h"

namespace {

const std::string kLink = "https://cloud.example.org/s/AbCdEfGhIjKlMnOp";

size_t code_points(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        const pbc::Utf8Char c = pbc::utf8_decode(s, i);
        i += c.len == 0 ? 1 : c.len;
        ++n;
    }
    return n;
}

}  // namespace

static_assert(!std::is_copy_constructible<pbc::MailHandoff>::value, "the arguments hold the link and are never copied");
static_assert(!std::is_copy_assignable<pbc::MailHandoff>::value, "the arguments hold the link and are never copied");
static_assert(std::is_nothrow_move_constructible<pbc::MailHandoff>::value, "the arguments can be moved");

PB_TEST(the_list_is_the_marker_the_text_its_length_and_the_subject_in_this_order) {
    pbc::MailBuild b = pbc::build_mail_handoff(std::string(kLink), "Link to Holiday photos");
    PB_CHECK(b.error == pbc::MailError::None);
    const std::vector<std::string>& a = b.handoff.argv();
    PB_CHECK_EQ(a.size(), size_t(4));
    PB_CHECK_EQ(a[0], std::string("pbo-mail-v1"));
    PB_CHECK_EQ(a[1], "text=" + kLink);
    PB_CHECK_EQ(a[2], "textlen=" + std::to_string(kLink.size()));
    PB_CHECK_EQ(a[3], std::string("subject=Link to Holiday photos"));
}

PB_TEST(there_is_no_recipient_key_and_no_other_key) {
    pbc::MailBuild b = pbc::build_mail_handoff(std::string(kLink), "Link to x");
    for (const std::string& arg : b.handoff.argv()) {
        PB_CHECK(arg.rfind("to=", 0) != 0);
    }
    PB_CHECK_EQ(b.handoff.argv().size(), size_t(4));
}

PB_TEST(the_text_length_counts_bytes_not_characters) {
    const std::string link = "https://cloud.example.org/s/\xc3\x84\xc3\xb6\xe2\x82\xac";   // "Äö€" is 7 bytes
    pbc::MailBuild b = pbc::build_mail_handoff(std::string(link), "s");
    PB_CHECK(b.error == pbc::MailError::None);
    PB_CHECK_EQ(b.handoff.argv()[1], "text=" + link);
    PB_CHECK_EQ(b.handoff.argv()[2], "textlen=" + std::to_string(link.size()));
}

PB_TEST(a_link_of_exactly_the_limit_is_accepted_and_one_byte_more_is_not) {
    const std::string prefix = "https://cloud.example.org/s/";
    std::string at_limit = prefix + std::string(pbc::kMailTextLimit - prefix.size(), 'a');
    PB_CHECK_EQ(at_limit.size(), pbc::kMailTextLimit);
    PB_CHECK(pbc::build_mail_handoff(std::move(at_limit), "s").error == pbc::MailError::None);
    std::string over = prefix + std::string(pbc::kMailTextLimit - prefix.size() + 1, 'a');
    pbc::MailBuild b = pbc::build_mail_handoff(std::move(over), "s");
    PB_CHECK(b.error == pbc::MailError::LinkTooLong);
    PB_CHECK(b.handoff.argv().empty());
}

PB_TEST(an_empty_link_is_refused) {
    pbc::MailBuild b = pbc::build_mail_handoff(std::string(), "s");
    PB_CHECK(b.error == pbc::MailError::LinkEmpty);
    PB_CHECK(b.handoff.argv().empty());
}

PB_TEST(a_link_with_a_control_character_or_invalid_utf8_is_refused) {
    for (const std::string& bad : {std::string("https://cloud.example.org/s/a\nb"), std::string("https://cloud.example.org/s/a\x01" "b"),
                                   std::string("https://cloud.example.org/s/a\x7f" "b"), std::string("https://cloud.example.org/s/a\xff" "b"),
                                   std::string("https://cloud.example.org/s/a\xc3")}) {
        pbc::MailBuild b = pbc::build_mail_handoff(std::string(bad), "s");
        PB_CHECK(b.error == pbc::MailError::LinkInvalid);
        PB_CHECK(b.handoff.argv().empty());
    }
}

PB_TEST(the_subject_is_one_line_without_control_characters_and_blank_runs) {
    PB_CHECK_EQ(pbc::clean_subject("Link to a\nb\r\nc\td"), std::string("Link to a b c d"));
    PB_CHECK_EQ(pbc::clean_subject("  Link   to   x  "), std::string("Link to x"));
    PB_CHECK_EQ(pbc::clean_subject(std::string("Link to a\x01" "b\x7f" "c")), std::string("Link to a b c"));
    PB_CHECK_EQ(pbc::clean_subject(""), std::string());
}

PB_TEST(the_subject_has_at_most_300_characters_and_is_cut_at_a_character_boundary) {
    const std::string name(400, 'x');
    const std::string cut = pbc::clean_subject("Link to " + name);
    PB_CHECK_EQ(code_points(cut), pbc::kMailSubjectLimit);
    PB_CHECK_EQ(cut.size(), pbc::kMailSubjectLimit);
    // a 4-byte character that would straddle the limit is dropped whole, never split
    std::string text = std::string(pbc::kMailSubjectLimit - 1, 'x') + "\xf0\x9f\x98\x80" + "tail";   // U+1F600 at characters 300..301
    const std::string cut2 = pbc::clean_subject(text);
    PB_CHECK_EQ(code_points(cut2), pbc::kMailSubjectLimit);
    PB_CHECK_EQ(cut2.size(), pbc::kMailSubjectLimit - 1 + 4);
    PB_CHECK(pbc::utf8_decode(cut2, cut2.size() - 4).valid);
    std::string text3 = std::string(pbc::kMailSubjectLimit, 'x') + "\xf0\x9f\x98\x80";
    PB_CHECK_EQ(pbc::clean_subject(text3).size(), pbc::kMailSubjectLimit);
}

PB_TEST(an_empty_item_name_gives_the_prefix_alone) {
    PB_CHECK_EQ(pbc::clean_subject("Link to "), std::string("Link to"));
}

PB_TEST(wipe_overwrites_the_bytes_of_every_string_and_empties_the_strings) {
    pbc::MailBuild b = pbc::build_mail_handoff(std::string(kLink), "Link to a subject that is long enough to live on the heap");
    PB_CHECK(b.error == pbc::MailError::None);
    std::vector<std::pair<const char*, size_t>> buffers;   // the strings stay in the list (emptied), so their heap buffers are still owned and readable
    for (const std::string& s : b.handoff.argv()) buffers.emplace_back(s.data(), s.size());
    b.handoff.wipe();
    for (const std::string& s : b.handoff.argv()) PB_CHECK(s.empty());
    for (const auto& buf : buffers) {
        if (buf.second < 24) continue;   // a short string lives inside the string object (small-string buffer), not on the heap: the loop above shows it empty
        for (size_t i = 0; i < buf.second; ++i) PB_CHECK(buf.first[i] == '\0');
    }
}

PB_TEST(a_moved_from_object_keeps_nothing_and_the_target_has_the_list) {
    pbc::MailBuild b = pbc::build_mail_handoff(std::string(kLink), "s");
    pbc::MailHandoff target(std::move(b.handoff));
    PB_CHECK_EQ(target.argv().size(), size_t(4));
    PB_CHECK(b.handoff.argv().empty());
}

PB_TEST_MAIN()
