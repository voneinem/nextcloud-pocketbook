// The rules that belong to the item model itself (specs/002-manage-server-files/data-model.md "State machine of a queue item"): the text
// forms of kinds and states used in the database, and which state may follow which. Pure.
#include <string>

#include "pbc/queue_types.h"
#include "test_support.h"

PB_TEST(kinds_and_states_have_stable_text_forms_that_round_trip) {
    for (pbc::ItemKind k : {pbc::ItemKind::Upload, pbc::ItemKind::Mkdir, pbc::ItemKind::Rename, pbc::ItemKind::Move, pbc::ItemKind::Copy, pbc::ItemKind::Delete}) {
        const auto back = pbc::parse_item_kind(pbc::item_kind_text(k));
        PB_CHECK(back.has_value() && *back == k);
    }
    for (pbc::ItemState s : {pbc::ItemState::Queued, pbc::ItemState::Sending, pbc::ItemState::Done, pbc::ItemState::Failed, pbc::ItemState::Conflict, pbc::ItemState::Unknown}) {
        const auto back = pbc::parse_item_state(pbc::item_state_text(s));
        PB_CHECK(back.has_value() && *back == s);
    }
    PB_CHECK_EQ(std::string(pbc::item_kind_text(pbc::ItemKind::Mkdir)), std::string("mkdir"));
    PB_CHECK_EQ(std::string(pbc::item_state_text(pbc::ItemState::Queued)), std::string("queued"));
    PB_CHECK(!pbc::parse_item_kind("trash").has_value());
    PB_CHECK(!pbc::parse_item_state("").has_value());
    PB_CHECK(!pbc::parse_item_state("Queued").has_value());
}

PB_TEST(only_the_documented_transitions_are_allowed) {
    using pbc::ItemState;
    const ItemState all[] = {ItemState::Queued, ItemState::Sending, ItemState::Done, ItemState::Failed, ItemState::Conflict, ItemState::Unknown};
    // queued -> sending; sending -> done | failed | conflict | unknown | queued (no connection, cancel, recovery)
    PB_CHECK(pbc::transition_allowed(ItemState::Queued, ItemState::Sending));
    for (ItemState to : {ItemState::Done, ItemState::Failed, ItemState::Conflict, ItemState::Unknown, ItemState::Queued}) PB_CHECK(pbc::transition_allowed(ItemState::Sending, to));
    // failed -> queued (Retry, Edit); conflict -> queued (a choice); unknown -> queued (Send again) | done (Mark as done)
    PB_CHECK(pbc::transition_allowed(ItemState::Failed, ItemState::Queued));
    PB_CHECK(pbc::transition_allowed(ItemState::Conflict, ItemState::Queued));
    PB_CHECK(pbc::transition_allowed(ItemState::Unknown, ItemState::Queued));
    PB_CHECK(pbc::transition_allowed(ItemState::Unknown, ItemState::Done));
    // an item is never taken from a held state straight into sending, and done is final
    for (ItemState from : {ItemState::Failed, ItemState::Conflict, ItemState::Unknown, ItemState::Done}) PB_CHECK(!pbc::transition_allowed(from, ItemState::Sending));
    for (ItemState to : all) PB_CHECK(!pbc::transition_allowed(ItemState::Done, to));
    PB_CHECK(!pbc::transition_allowed(ItemState::Queued, ItemState::Done));       // not without having been sent
    PB_CHECK(!pbc::transition_allowed(ItemState::Failed, ItemState::Done));
    PB_CHECK(!pbc::transition_allowed(ItemState::Conflict, ItemState::Done));
    // a state to itself is not a transition
    for (ItemState s : all) PB_CHECK(!pbc::transition_allowed(s, s));
}

PB_TEST(a_held_state_is_one_a_run_never_takes) {
    PB_CHECK(pbc::is_held(pbc::ItemState::Failed));
    PB_CHECK(pbc::is_held(pbc::ItemState::Conflict));
    PB_CHECK(pbc::is_held(pbc::ItemState::Unknown));
    PB_CHECK(!pbc::is_held(pbc::ItemState::Queued));
    PB_CHECK(!pbc::is_held(pbc::ItemState::Sending));
    PB_CHECK(!pbc::is_held(pbc::ItemState::Done));
}

PB_TEST_MAIN()
