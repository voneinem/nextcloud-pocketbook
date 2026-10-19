# Testing Walkaway

Walkaway is verified in two ways (Constitution VII): automated invariant checks with
pytest, and a manual protocol run in every supported agent.

## Automated checks

From the repository root:

```powershell
uv run --no-project --with pytest --with "specify-cli>=0.12.17" pytest tests -q
```

The tests cover invariants M1–M13, C1–C6, and P1–P3 from
[specs/001-walkaway-phase-commands/contracts/manifest.md](../specs/001-walkaway-phase-commands/contracts/manifest.md).
CI runs the same command on every push and pull request.

## Dispatch spike results

Research R1 decides how a command body makes the agent run another installed command.
Three probe commands dispatch core analyze and then print
`WALKAWAY-PROBE: continued after analyze`:

- **a**: "Invoke `/speckit-analyze` exactly as if the developer had typed it in this
  chat."
- **b**: "Open the installed command or skill definition named after `/speckit-analyze`
  for this project's agent and follow it completely."
- **ab**: the R1 wording, (a) with the (b) fallback, "in this conversation, not in a
  sub-agent".

**Date**: 2026-10-15 · **Spec Kit**: 1.2.0 · **Decision**: **pending live runs**
(provisional: R1 wording, (a) first)

Static checks (done):

| Sample | Init | Probe skills rendered | Tokens left | Core analyze `disable-model-invocation` |
|---|---|---|---|---|
| copilot | `--integration copilot --script ps` | `speckit-wkprobe-{a,b,ab}` contain `/speckit-analyze` | 0 | n/a |
| claude | `--integration claude --script ps` | same | 0 | `false` |
| claude-nmi | `--integration claude --integration-options="--no-model-invocation"` | same | 0 | `true` |

Live runs (to do; new chat or session per probe). Record Y/N for: analyze report in the
same conversation · marker printed after it · sub-agent used · `git status` clean.

| Agent (version) | Sample | a | b | ab |
|---|---|---|---|---|
| VS Code Copilot Chat (—) | copilot | — | — | — |
| Claude Code (—) | claude | — | — | — |
| Claude Code (—) | claude-nmi | — | — | — |

Rules: **GO (R1 wording)** if `ab` dispatches and continues on every sample. **GO
(fallback-first)** if (a) fails on an agent but (b) works: swap the sentence order of
the dispatch rule in both command bodies. **NO-GO** if neither works on an agent: the
spec must be revised.

To reproduce the probe, create an extension with three commands whose bodies are the
wordings above (using the `__SPECKIT_COMMAND_ANALYZE__` token), install it with
`specify extension add --dev` into a sample that has a spec, plan, and tasks, and run
each probe.

## Manual test protocol

This is the release gate (Constitution VII, SC-008). Run it once per agent.

### Setup

`--ai` was removed from `specify init` in 0.10.0; use `--integration`.

```powershell
$root = Join-Path $env:TEMP 'walkaway-manual'; New-Item -ItemType Directory $root -Force | Out-Null
Set-Location $root
specify init sample-copilot --integration copilot --script ps --ignore-agent-tools
specify init sample-claude  --integration claude  --script ps --ignore-agent-tools
foreach ($p in 'sample-copilot','sample-claude') {
  Push-Location $p; git init -q; git add -A; git commit -qm init
  specify extension add --dev D:\Projects\tools\spec-kit-walkaway
  specify extension list            # expect: Walkaway (v<version>) · Commands: 2 · Hooks: 0
  Pop-Location
}
```

Expected after install:

- `specify extension list` shows Walkaway with 2 commands and 0 hooks.
- The rendered skills `speckit-walkaway-define` and `speckit-walkaway-prepare` contain
  `/speckit-plan` and the other core names, with no `__SPECKIT_COMMAND_` tokens left.
- `.specify/extensions/walkaway/` contains no `tests/`, `specs/`, `.github/`, `.specify/`,
  or `.git/`. A `--dev` install copies `.git/` unless `.extensionignore` lists it; on
  Windows the copied read-only git objects then make `specify extension remove` fail.

Open each sample in its agent (`code sample-copilot` → Copilot Chat in Agent mode;
`cd sample-claude; claude`). "Run X" means type the rendered command, for example
`/speckit-walkaway-define …`.

### Approval configuration

Before scenarios 4–12, configure the narrow approvals from
[usage.md](usage.md#running-unattended) in each sample so that agent approval prompts do
not pause the unattended steps.

### Scenarios

| # | Scenario | Steps | Expected outcome |
|---|---|---|---|
| 0 | Dispatch check | Install the probe (see above) in a sample with spec, plan, and tasks; then repeat with the real commands. | Core analyze runs in the same conversation; the run continues after it. |
| 1 | Missing constitution | Run define with "Photo album app". | Stops immediately, names the constitution command, `git status` clean. |
| 2 | Missing spec | Run prepare. | Stops and names define. `git status` clean. |
| 3 | Invalid rounds | Run prepare with `7`, then with `two`. | Both stop with the 1–5 message before anything else. |
| 4 | Happy path define | Fill in the constitution, commit, run define with a short description, answer the questions. | Specify asks nothing; clarify asks one at a time; 6-heading summary ≤ ~40 lines; Next step = prepare; no plan.md; no `[NEEDS CLARIFICATION:` left after all answers. |
| 5 | Define re-run | (a) define with no description; (b) with the same description; (c) with a new description. | Only clarify questions. (a), (b): clarify on the same feature, no new directory. (c): new feature directory. |
| 6 | Happy path prepare | Run prepare with no argument; do not touch the chat. | plan.md, tasks.md, and analysis with no question until the findings report; findings grouped with counts. Record every approval prompt (target: none). |
| 7 | Decline fixes | Answer `no`. | `git status` shows only scenario 6 files. Next step = implement. |
| 8 | Accept fixes | Commit, run prepare with `2`, answer `yes`. | At most 2 rounds; one line per fix grouped by round; only feature files changed; constitution untouched; implement not started. |
| 9 | Prepare re-run | Commit, run prepare again. | No duplicate directory or files; plan.md and tasks.md listed as replaced with the version-control reminder. |
| 10 | Implementation started | Mark one task `[x]`, run prepare. | Stops, explains implementation started; `git diff` shows only the `[x]` edit. |
| 11 | Unclear fix answer | At the fix question, answer `maybe`. | Asks once more; nothing changes until yes or no. |
| 12 | Open markers | Add `[NEEDS CLARIFICATION: test]` to spec.md, commit, run prepare. | Stops before any change, lists the marker, names define without a description. `git status` clean. |

**Pass criteria**: every scenario behaves as expected on both agents.

### Cleanup

```powershell
Remove-Item (Join-Path $env:TEMP 'walkaway-manual') -Recurse -Force
```

## Results

| Scenario | Copilot Chat result | Claude Code result | Date | Agent version | Spec Kit version |
|---|---|---|---|---|---|
| install (static) | pass: 2 commands, 0 hooks, 0 tokens left | pass: same | 2026-10-15 | — | 1.2.0 |
| floor (static) | pass: agent-file mode, `/speckit.plan` etc., 0 tokens left | pass: skills, `/speckit-plan` etc., 0 tokens left | 2026-10-15 | — | 0.12.17 |
| 0 | — | — | | | |
| 1 | — | — | | | |
| 2 | — | — | | | |
| 3 | — | — | | | |
| 4 | — | — | | | |
| 5 | — | — | | | |
| 6 | — | — | | | |
| 7 | — | — | | | |
| 8 | — | — | | | |
| 9 | — | — | | | |
| 10 | — | — | | | |
| 11 | — | — | | | |
| 12 | — | — | | | |
