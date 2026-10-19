---
description: "Phase 1: create and clarify a feature spec, then stop for review"
---

## User Input

```text
$ARGUMENTS
```

The input is an optional free-text feature description. Trim it before use.

## Operating rules

- **Dispatch**: to dispatch a command, invoke it exactly as if the developer had typed it
  in this chat. If you cannot invoke commands yourself, open the installed command or
  skill definition named after it for this project's agent and follow it completely.
  Either way, run it in this conversation, not in a sub-agent.
- A core command's completion report or suggested next step ends a *step*, not the
  phase. Continue with the next step of this command without waiting.
- Never modify core command or skill files. Leave extension hooks that core commands
  run exactly as core defines them; do not act on optional hook suggestions.
- The only questions the developer may be asked are the clarification questions of
  Step 2. Ask nothing else, including on re-runs.
- **Failure rule**: if any check or step fails, stop at once and run no later step.
  Keep every file already produced; roll nothing back. Show the Review Summary with
  Status "stopped", naming the steps that finished, the step that failed and why, and
  as Next step the re-run command `__SPECKIT_COMMAND_WALKAWAY_DEFINE__ <description>`.

## Entry checks

Run these checks in order before changing any file. A failed check shows the Review
Summary with Status "stopped — <check>", lists no files, and changes nothing.

1. **D1 — initialised**: the project root has a `.specify/` directory. Otherwise stop
   with Status "stopped — not initialised" and Next step "run `specify init` in a
   terminal".
2. **D2 — ratified constitution**: `.specify/memory/constitution.md` exists and contains
   no unfilled template placeholder, i.e. no bracketed upper-case token of two or more
   characters such as `[PROJECT_NAME]` or `[PRINCIPLE_1_NAME]` (pattern
   `\[[A-Z][A-Z0-9_]+\]`; checkbox marks like `[X]` do not count). A `TODO(...)` marker
   is allowed. Otherwise stop with Status "stopped — no ratified constitution" and
   Next step `__SPECKIT_COMMAND_CONSTITUTION__`.
3. **D3 — existing spec**: read `feature_directory` from `.specify/feature.json`. If the
   file exists and `<feature_directory>/spec.md` exists, choose the mode from the input
   and **never ask**:
   - **re-clarify** when the description is empty, or when it equals, trimmed and
     case-insensitive, the quoted text of the spec's `**Input**: User description: "…"`
     line (read `\"` in that line as `"`). Skip Step 1 and run Step 2 on the current
     feature.
   - **new feature** for any other description. Run Step 1 normally.
   - If spec.md has no such input line, any non-empty description means **new
     feature**, and the Status line says "description not compared: spec.md has no
     input line".

   State the chosen mode on the Status line. If there is no current spec, go to D4.
4. **D4 — description present**: when there is no current spec and the description is
   empty, stop with Status "stopped — description missing" and Next step
   `__SPECKIT_COMMAND_WALKAWAY_DEFINE__ <feature description>`.

## Step 1 — Specify

Skipped in mode re-clarify.

Dispatch `__SPECKIT_COMMAND_SPECIFY__` with the description as its input. Override:

- Skip the step that presents `[NEEDS CLARIFICATION]` questions to the user. Leave every
  marker in spec.md for clarify. Ask the developer nothing during this step.
- Feature-branch handling stays whatever the core command does.
- Its completion report and suggested next command end this step; continue with Step 2.

## Step 2 — Clarify

Dispatch `__SPECKIT_COMMAND_CLARIFY__` on the current feature. Overrides:

- Cover the `[NEEDS CLARIFICATION: …]` markers left in spec.md first, then any other
  ambiguities clarify finds.
- Ask one question at a time and wait for each answer before the next question.
- For every answered marker, replace the whole `[NEEDS CLARIFICATION: …]` text in
  spec.md with the decided wording. Leave a marker only when its question was skipped
  or not asked.
- The answers "skip", "done", "stop", and "proceed" end clarification early. The
  unanswered points become open points.
- Ignore clarify's "Suggested next command".
- If clarify reports no critical ambiguities, ask nothing and continue to the summary.

## Define summary

Fill the Review Summary from the current feature's spec.md:

- **Status**: "completed — <mode>; scope: N user stories, M functional requirements".
  N is the number of `### User Story` headings, M the number of `**FR-` items.
- **Files**: links to spec.md and to the checklist written by specify, each marked
  created or updated.
- **Decisions**: one `Q: … → A: …` line per answer recorded in this run's
  `## Clarifications` session.
- **Open points**: remaining `[NEEDS CLARIFICATION]` markers, and the Outstanding or
  Deferred categories reported by clarify.
- **Needs your decision**: none.
- **Next step**: exactly `__SPECKIT_COMMAND_WALKAWAY_PREPARE__`.

## Review Summary

Every stop point (completion, failed check, failed step) shows this summary in chat. The
headings are fixed and always appear in this order. A heading with no content shows
`none`. Keep it to about 40 lines and link to files rather than copying their content.

```markdown
## Walkaway define — Review

**Status**: <completed | stopped> — <one-line reason>

**Files**
- <created|updated>: [path](path)

**Decisions**
- Q: <question> → A: <answer>

**Open points**
- <open point>

**Needs your decision**
- none

**Next step**: <exactly one command>
```

## STOP

After the Review Summary, stop. This command never starts another phase: do not
dispatch plan, tasks, analyze, or implement, and do not run the Next step yourself. The
developer decides when to continue.
