---
name: speckit-walkaway-prepare
description: 'Phase 2: plan, tasks and analyze unattended, then optional bounded fixes'
compatibility: Requires spec-kit project structure with .specify/ directory
metadata:
  author: <author>
  source: walkaway:commands/prepare.md
---

## User Input

```text
$ARGUMENTS
```

The input is an optional round limit for the fix loop.

## Operating rules

- **Dispatch**: to dispatch a command, invoke it exactly as if the developer had typed it
  in this chat. If you cannot invoke commands yourself, open the installed command or
  skill definition named after it for this project's agent and follow it completely.
  Either way, run it in this conversation, not in a sub-agent.
- A core command's completion report or suggested next step ends a *step*, not the
  phase. Continue with the next step of this command without waiting.
- Never modify core command or skill files. Leave extension hooks that core commands
  run exactly as core defines them; do not act on optional hook suggestions.
- Ask the developer nothing except the single fix question at the review point (and one
  re-ask if the answer is unclear).
- **Failure rule**: if any step fails, including an analyze re-run in the fix loop, stop
  at once and run no later step. Keep every file already produced; roll nothing back.
  Show the Review Summary with Status "stopped", naming the steps that finished, the
  step that failed and why, and as Next step the re-run command
  `/speckit-walkaway-prepare`.

## Input

Trim the input. Empty means a round limit of 3. A single whole number from 1 to 5 sets
the limit for this run only; it is never stored. Any other value stops the command
before any check or step, with Status "stopped — Round limit must be a whole number from
1 to 5 (default 3)." and Next step `/speckit-walkaway-prepare`.

## Entry checks

Run these checks in order before changing any file. A failed check shows the Review
Summary with Status "stopped — <check>", lists no files, and changes nothing.

1. **P2 — spec exists**: read `feature_directory` from `.specify/feature.json` and
   require `<feature_directory>/spec.md`. If the file or the spec is missing, stop with
   Status "stopped — no current feature specification" and Next step
   `/speckit-walkaway-define <feature description>`.
2. **P2b — no open clarifications**: if spec.md contains the literal text
   `[NEEDS CLARIFICATION:` (with the colon) outside fenced code blocks and inline code
   spans, stop with Status "stopped — open clarifications". List each open marker under
   **Needs your decision**. Next step: `/speckit-walkaway-define` with no
   description, which re-runs clarification; mention editing spec.md by hand as the
   alternative. Do not ask anything.
3. **P3 — implementation not started**: if tasks.md exists and contains a checkbox line
   `- [x]` or `- [X]` outside code fences, implementation has already started. Stop
   with Status "stopped — implementation already started" and Next step
   `/speckit-implement`. Modify no artifact.

## Steps

Run these steps in order without asking the developer anything.

- **S0 — prepare regeneration**: record which of `plan.md`, `research.md`,
  `data-model.md`, `quickstart.md`, `contracts/`, and `tasks.md` already exist in the
  feature directory. If `plan.md` exists, delete **only** `plan.md`, so the plan step
  starts from a fresh template. Use a file operation rather than a shell command where
  one is available. List every pre-existing file from that set under **Files** as
  "replaced", followed by "Earlier versions are recoverable from version control." If a
  later step fails after `plan.md` was deleted, **Files** lists "`plan.md` — deleted;
  recover the earlier version from version control".
- **S1 — plan**: dispatch `/speckit-plan`.
- **S2 — tasks**: dispatch `/speckit-tasks`.
- **S3 — analyze**: dispatch `/speckit-analyze`. Skip its "Offer
  Remediation" question; do not ask it. Collect its findings table (ID, Category,
  Severity, Location, Summary).

## Review point

Group the findings under CRITICAL, HIGH, MEDIUM, and LOW with a count per group. The
report is **clean** when it has no CRITICAL or HIGH findings; MEDIUM and LOW are still
listed.

- **Clean**: show the Review Summary with Status "completed — analysis clean", ask no
  question, Next step `/speckit-implement`, then STOP.
- **Not clean**: show the Review Summary with Status "completed — analysis done" and,
  as Next step, the single question "Apply fixes? (yes/no)". Then wait for the answer.

The summary is shown in chat only. Never write a report file.

## Fix decision

- Accept a clear yes or no. On any other answer, ask once more for an explicit yes or
  no, and change nothing until the answer is clear.
- **no**: modify no feature artifact. Show the final summary with Status "completed —
  fixes declined", Next step `/speckit-implement`, then STOP.
- **yes**: run the fix loop.

## Fix loop

Ask the developer nothing inside the loop. Repeat rounds, starting at round 1:

1. Attempt every finding, at every severity, that is not marked needs-decision.
   - Edit only these files in the current feature directory: `spec.md`, `plan.md`,
     `research.md`, `data-model.md`, `quickstart.md`, `contracts/**`, and `tasks.md`.
     Never edit the constitution, anything under `.specify/`, agent command or skill
     files, or `checklists/`.
   - If a fix needs a product or scope decision, a choice between valid requirement
     options, or a constitution amendment, leave it unchanged and mark the finding
     "needs your decision".
   - Record one fix line per change: finding, file, short change.
2. Re-dispatch `/speckit-analyze` and skip its remediation offer. If it
   fails, apply the failure rule.
3. Carry needs-decision marks forward: a new finding with the same category, location,
   and summary gist as a marked one stays marked.
4. Stop the loop when no CRITICAL or HIGH findings remain, when every remaining CRITICAL
   or HIGH finding is marked needs-decision, or when the round number equals the limit.
   Otherwise start the next round.

## Final summary

Fill the Review Summary after the fix loop:

- **Status**: "completed — rounds: N of LIMIT; <stop reason>".
- **Files**: replaced files from S0 and files changed by fixes.
- **Decisions**: one fix line per change, grouped under `Round n`. If this would push
  the summary past about 40 lines, show the remainder as a count per file.
- **Findings by severity**: from the last analyze run.
- **Needs your decision**: the findings marked needs-decision, each with why.
- **Next step**: `/speckit-implement`.

Shown in chat only, then STOP.

## Review Summary

Every stop point (findings report, final summary, failed check, failed step) shows this
summary in chat. The headings are fixed and always appear in this order. A heading with
no content shows `none`. Keep it to about 40 lines and link to files rather than copying
their content.

```markdown
## Walkaway prepare — Review

**Status**: <completed | stopped> — <one-line reason>[; rounds: N of LIMIT]

**Files**
- <created|updated|replaced|deleted>: [path](path)
- Earlier versions are recoverable from version control. (only when files were replaced or deleted)

**Decisions**
- Round 1: <finding> — <file> — <short change>

**Findings by severity**
- CRITICAL (n): …
- HIGH (n): …
- MEDIUM (n): …
- LOW (n): …

**Needs your decision**
- <finding> — <why a decision is needed>

**Next step**: <exactly one command> | Apply fixes? (yes/no)
```

## STOP

After the Review Summary, stop. Never start implementation: do not dispatch implement or
any other phase, and do not run the Next step yourself. The developer decides when to
continue.