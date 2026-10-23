// SecretText (specs/004-share-links, contracts/core-interfaces.md): a secret string that is move-only, wiped when it dies or on request, and
// that cannot be copied, streamed or turned into a plain string by accident.
#include <cstring>
#include <new>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>

#include "pbc/secret_text.h"
#include "test_support.h"

namespace {

template <class T, class = void>
struct has_ostream : std::false_type {};
template <class T>
struct has_ostream<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>> : std::true_type {};

template <class T, class = void>
struct has_to_string : std::false_type {};
template <class T>
struct has_to_string<T, std::void_t<decltype(std::to_string(std::declval<const T&>()))>> : std::true_type {};

const std::string kSecret = "Zx9-secret-pw-which-is-long-enough-to-live-on-the-heap";

}  // namespace

static_assert(!std::is_copy_constructible<pbc::SecretText>::value, "a secret is never copied");
static_assert(!std::is_copy_assignable<pbc::SecretText>::value, "a secret is never copied");
static_assert(std::is_nothrow_move_constructible<pbc::SecretText>::value, "a secret can be moved");
static_assert(!std::is_constructible<pbc::SecretText, const std::string&>::value, "taking a secret from an lvalue would leave a copy behind");
static_assert(!has_ostream<pbc::SecretText>::value, "a secret has no stream operator");
static_assert(!has_to_string<pbc::SecretText>::value, "a secret has no to_string");

PB_TEST(a_new_secret_holds_the_value_and_reports_its_size) {
    std::string value = kSecret;
    pbc::SecretText s(std::move(value));
    PB_CHECK(!s.empty());
    PB_CHECK_EQ(s.size(), kSecret.size());
    PB_CHECK_EQ(s.reveal(), kSecret);
}

PB_TEST(wipe_overwrites_the_buffer_and_empties_the_secret) {
    pbc::SecretText s{std::string(kSecret)};
    const char* p = s.reveal().data();   // the heap buffer stays owned by the object, so reading it after wipe() is defined
    const size_t n = kSecret.size();
    s.wipe();
    PB_CHECK(s.empty());
    PB_CHECK_EQ(s.size(), size_t(0));
    for (size_t i = 1; i < n; ++i)   // byte 0 is the terminator written by clear(); all others must have been overwritten
        if (p[i] != 0) PB_FAIL_AT("byte " + std::to_string(i) + " of the buffer still holds secret data");
}

PB_TEST(the_destructor_wipes_a_short_secret_that_lives_inside_the_object) {
    // a short string is stored inside the std::string object itself, so its bytes can be inspected in the raw storage after the destructor ran
    alignas(pbc::SecretText) unsigned char storage[sizeof(pbc::SecretText)];
    const std::string pw = "pw-short";
    auto* s = new (storage) pbc::SecretText{std::string(pw)};
    s->~SecretText();
    for (size_t i = 0; i + pw.size() <= sizeof(storage); ++i)
        if (std::memcmp(storage + i, pw.data(), pw.size()) == 0) PB_FAIL_AT("the short secret is still in the destroyed object");
}

PB_TEST(a_moved_from_secret_holds_nothing_and_the_target_holds_the_value) {
    pbc::SecretText a{std::string(kSecret)};
    pbc::SecretText b(std::move(a));
    PB_CHECK(a.empty());
    PB_CHECK_EQ(a.size(), size_t(0));
    PB_CHECK_EQ(b.reveal(), kSecret);
    pbc::SecretText c;
    PB_CHECK(c.empty());
    c = std::move(b);
    PB_CHECK(b.empty());
    PB_CHECK_EQ(c.reveal(), kSecret);
}

PB_TEST(move_assignment_replaces_the_value) {
    pbc::SecretText a{std::string(kSecret)};
    pbc::SecretText b{std::string("other-secret-that-is-also-long-enough-for-the-heap-xx")};
    a = std::move(b);
    PB_CHECK_EQ(a.reveal(), std::string("other-secret-that-is-also-long-enough-for-the-heap-xx"));
    PB_CHECK(b.empty());
}

PB_TEST(an_empty_secret_is_empty) {
    pbc::SecretText s;
    PB_CHECK(s.empty());
    PB_CHECK_EQ(s.size(), size_t(0));
    s.wipe();   // wiping an empty secret is fine
    PB_CHECK(s.empty());
}

PB_TEST(content_checks_need_no_reveal) {
    PB_CHECK(!pbc::SecretText{std::string("plain pw")}.has_control_character());
    PB_CHECK(pbc::SecretText{std::string("a\nb")}.has_control_character());
    PB_CHECK(pbc::SecretText{std::string("a\x01" "b")}.has_control_character());
    PB_CHECK(pbc::SecretText{std::string("a\x7f" "b")}.has_control_character());
    PB_CHECK(!pbc::SecretText{std::string("\xc3\xa4\xc3\xb6")}.has_control_character());   // umlauts are fine
    PB_CHECK(pbc::SecretText{std::string("  \t ")}.all_blank());
    PB_CHECK(pbc::SecretText{}.all_blank());
    PB_CHECK(!pbc::SecretText{std::string(" x ")}.all_blank());
}

PB_TEST_MAIN()
