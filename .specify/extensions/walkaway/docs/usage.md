# Using Walkaway

Walkaway adds two chat commands to a Spec Kit project. Each runs one phase of
spec-driven development and stops at a review point. Commands are written here in
dotted form; type them the way your agent lists them, for example
`/speckit-walkaway-define` in Copilot Chat skills mode and Claude Code.

## The three phases

| Phase | Command | Runs | Asks you |
|---|---|---|---|
| 1 | `speckit.walkaway.define <description>` | core specify, then core clarify | clarification questions only |
| 2 | `speckit.walkaway.prepare [rounds]` | core plan, tasks, analyze, then optional fixes | "Apply fixes? (yes/no)" only |
| 3 | core `speckit.implement` | implementation | whatever implement asks |

Walkaway never starts phase 3, and never starts the next phase on its own.

## Inputs

- **define**: a free-text feature description. Optional when the current feature
  already has a spec (see re-run rules).
- **prepare**: an optional round limit for the fix loop, a whole number from 1 to 5.
  The default is 3. The value applies to this run only. Anything else stops the command
  with "Round limit must be a whole number from 1 to 5 (default 3)."

## The review summary

Every stop shows the same headings in this order: **Status**, **Files**,
**Decisions**, **Open points** (define) or **Findings by severity** (prepare),
**Needs your decision**, **Next step**. Empty headings show `none`. The summary stays
within about 40 lines and links to files instead of repeating them. It is shown in chat
only; no report file is written.

## Stop points

| Command | Stop | Next step |
|---|---|---|
| define | `.specify/` missing | run `specify init` in a terminal |
| define | constitution missing or still the unfilled template | the constitution command |
| define | no current spec and no description | define with a description |
| define | completed | prepare |
| prepare | invalid round limit | prepare with a valid limit |
| prepare | no current feature spec | define with a description |
| prepare | `[NEEDS CLARIFICATION: …]` markers in spec.md | define with no description (re-runs clarify), or edit spec.md |
| prepare | a task in tasks.md is marked done | implement |
| prepare | analysis has critical or high findings | "Apply fixes? (yes/no)" |
| prepare | clean analysis, fixes declined, or fix loop ended | implement |
| either | a step failed | the same command again; finished files are kept |

## Re-run rules

- **define** never asks anything besides clarification questions:
  - no description, or the same description as the spec's `**Input**: User
    description: "…"` line (trimmed, case-insensitive): re-runs clarify on the current
    feature, no new feature.
  - any other description: starts a new feature.
  - Limitation: the same-description check needs the `**Input**:` line that the default
    spec template writes. If your spec has no such line, any description starts a new
    feature, and the summary says no comparison was possible.
- **prepare** regenerates `plan.md` and `tasks.md` from the current spec and re-runs
  analysis. It deletes only `plan.md` before planning, overwrites the rest, and lists
  every replaced file with a reminder that earlier versions are in version control.
  Commit before re-running if you edited those files by hand.
- **prepare** refuses to run when any task is marked `[x]`, because implementation has
  started, and when the spec still has `[NEEDS CLARIFICATION: …]` markers.

## The fix loop

After "yes", each round tries to fix every finding at every severity, then re-runs
analyze. It stops when no critical or high findings remain, when only findings marked
"needs your decision" remain at critical or high, or at the round limit.

- Fixes touch only the current feature's `spec.md`, `plan.md`, `research.md`,
  `data-model.md`, `quickstart.md`, `contracts/`, and `tasks.md`. The constitution,
  `.specify/`, agent files, and `checklists/` are never edited.
- A finding that needs a product or scope decision, a choice between valid options, or
  a constitution change is left alone and listed under **Needs your decision**.
- **Decisions** lists one line per fix, grouped by round.
- "no" changes nothing. An unclear answer is asked once more.

## Running unattended

Walkaway itself asks nothing during the unattended steps, but your agent may ask you to
approve tool calls: the `.specify/scripts/` commands that core plan, tasks, and analyze
run, file edits, and the deletion of `plan.md`. Each approval prompt pauses the run.
Approve narrowly instead of turning on blanket auto-approval.

### VS Code Copilot Chat

Add terminal auto-approve rules for the project's Spec Kit scripts only, for example in
the workspace settings:

```jsonc
{
  "chat.tools.terminal.autoApprove": {
    "/^(pwsh|powershell)(\\.exe)?\\s.*\\.specify[\\\\/]scripts[\\\\/]powershell[\\\\/][a-z-]+\\.ps1\\b/": true,
    "/^\\.specify[\\\\/]scripts[\\\\/](powershell|bash)[\\\\/][a-z-]+\\.(ps1|sh)\\b/": true
  }
}
```

Adjust the patterns to the script type you chose at `specify init`. Check the setting
name in your VS Code version (Settings → search "terminal auto approve").

### Claude Code

Add a `permissions.allow` list to the project's `.claude/settings.json` for the scripts
and for edits inside `specs/`, for example:

```json
{
  "permissions": {
    "allow": [
      "Bash(.specify/scripts/bash/*)",
      "Bash(pwsh .specify/scripts/powershell/*)",
      "Edit(specs/**)",
      "Write(specs/**)"
    ]
  }
}
```

Do not use a bypass-all permission mode for unattended runs. Check the rule syntax
against the permissions documentation of your Claude Code version.

## Troubleshooting

- **"no current feature" although a spec exists**: walkaway reads the current feature
  from `.specify/feature.json`. With `SPECIFY_FEATURE_NO_PERSIST` set, that file is not
  written. Unset it, or run core specify once so the pointer is saved.
- **Claude Code project initialised with `--no-model-invocation`**: the agent cannot
  invoke core skills itself, so walkaway tells it to open the installed skill definition
  and follow it. This is slower but works the same.
- **Hooks of other extensions run during a phase**: core commands run their hooks as
  usual. Walkaway neither adds nor suppresses them.
- **The run still paused for approval**: an agent approval prompt, not a walkaway
  question. Add a narrow rule as described above.
