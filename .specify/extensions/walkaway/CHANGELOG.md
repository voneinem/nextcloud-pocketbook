# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `speckit.walkaway.define`: core specify with no questions, then interactive clarify, then a review summary and stop.
- `speckit.walkaway.prepare`: core plan, tasks, and analyze unattended, findings grouped by severity, and one fix question.
- Bounded fix loop after a yes: 1–5 rounds per run, default 3; findings needing a product decision are listed separately.
- Re-run safety: define decides re-clarify or new feature from its input; prepare regenerates plan and tasks, lists replaced files, and refuses when tasks are done or clarifications are open.
- Fixed six-heading review summary at every stop point.
- Support for VS Code Copilot Chat and Claude Code.
