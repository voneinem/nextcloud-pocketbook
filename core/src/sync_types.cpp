#include "pbc/sync_types.h"

namespace pbc {

const char* direction_text(Direction d) {
    switch (d) {
    case Direction::Down: return "down";
    case Direction::Up: return "up";
    case Direction::Both: return "both";
    }
    return "down";
}

std::optional<Direction> parse_direction(std::string_view text) {
    if (text == "down") return Direction::Down;
    if (text == "up") return Direction::Up;
    if (text == "both") return Direction::Both;
    return std::nullopt;
}

const char* plan_action_text(PlanAction a) {
    switch (a) {
    case PlanAction::Download: return "download";
    case PlanAction::Upload: return "upload";
    case PlanAction::Skip: return "skip";
    case PlanAction::KeepBoth: return "keep_both";
    case PlanAction::ProposeDeleteServer: return "propose_delete_server";
    case PlanAction::ProposeDeleteReader: return "propose_delete_reader";
    }
    return "skip";
}

std::optional<PlanAction> parse_plan_action(std::string_view text) {
    for (PlanAction a : {PlanAction::Download, PlanAction::Upload, PlanAction::Skip, PlanAction::KeepBoth, PlanAction::ProposeDeleteServer,
                         PlanAction::ProposeDeleteReader})
        if (text == plan_action_text(a)) return a;
    return std::nullopt;
}

const char* choice_text(Choice c) {
    switch (c) {
    case Choice::None: return "";
    case Choice::TransferAgain: return "transfer_again";
    case Choice::Leave: return "leave";
    case Choice::Delete: return "delete";
    }
    return "";
}

std::optional<Choice> parse_choice(std::string_view text) {
    for (Choice c : {Choice::None, Choice::TransferAgain, Choice::Leave, Choice::Delete})
        if (text == choice_text(c)) return c;
    return std::nullopt;
}

std::string rel_join(std::string_view parent_rel, std::string_view name) {
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos ||
        name.find('\0') != std::string_view::npos)
        return std::string();
    std::string out(parent_rel);
    if (!out.empty()) out.push_back('/');
    out.append(name);
    return out;
}

std::string remote_prefix(const std::string& folder) {
    if (folder.empty() || folder == "/") return "/";
    std::string out = folder;
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    out.push_back('/');
    return out;
}

bool plan_transition_allowed(const std::string& from, const std::string& to) {
    struct T {
        const char* from;
        const char* to;
    };
    static const T kLegal[] = {{"ready", "approved"},   {"ready", "cancelled"},      {"ready", "superseded"},
                               {"approved", "running"}, {"approved", "interrupted"}, {"running", "done"},
                               {"running", "interrupted"}, {"interrupted", "superseded"}};
    for (const T& t : kLegal)
        if (from == t.from && to == t.to) return true;
    return false;
}

}  // namespace pbc
