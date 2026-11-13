---
name: codex-spend-tracker
description: Track cumulative Codex token usage from local session logs, report changes since the previous update, and show input, cached input, and output totals. Use for token usage tracking and running usage summaries.
---

# Codex token tracker

When ongoing tracking is enabled, run near the end of each turn:

```sh
python3 skills/codex-spend-tracker/scripts/track_usage.py --project .
```

Resolve the script relative to this skill when invoked elsewhere. `--project` selects the workspace, defaulting to the current directory. The helper reads `$CODEX_HOME/sessions` or `~/.codex/sessions`, filters by the exact project path in session metadata, and stores session IDs and cumulative counters in `.usage-tracker/usage.sqlite3`. It does not store prompt contents or read credentials. `--sessions PATH` supports exported logs; `--json` produces structured output.

Append a short usage line showing the increment and cumulative total. Input includes cached input; do not add cached input again. Output includes reasoning where reported; do not add reasoning separately. These are model-call tokens, including repeated context and tool calls, rather than just typed prompts. The current final response and any unflushed usage appear on the next refresh. First use includes all available project history. Deleted logs remain represented in the ledger.

The user uses a GPT subscription and wants tokens, not money. Do not infer billed spending or quota percentages. Missing snapshots mean unavailable usage, not confirmed zero. Report warnings about unreadable logs, unsupported counters, and counter decreases as possible incompleteness. Repeated scans are idempotent; sessions are deduplicated by ID and cumulative counters.

A skill does not install a background hook. The root AGENTS.md supplies the per-response instruction for this project. Elsewhere, explicitly invoke the skill or enable the equivalent project instruction.
