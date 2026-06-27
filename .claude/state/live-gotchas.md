# Live gotchas -- transient hazards the overnight loop should know BEFORE it hits them.
# Format: "- YYYY-MM-DD: <hazard> -> <what to do>" with optional "(expires YYYY-MM-DD)".
# Read by .claude/hooks/runner_status.py; expired lines are dropped from the brief.

- 2026-06-14: codex_review_completed recorder can mis-fire / not fire -> a Bash command merely CONTAINING the dispatch-script name can set last-codex-review.json received=false and block the next edit. If no real Codex was dispatched, correct the state (received=true) and proceed; never perform a fake review. (project_codex_review_hook_dead)
- 2026-06-27: scripts/lsp-mcp/bridge.py rework in flight -> lsp-bridge may disconnect mid-session; fall back to grep and do not edit bridge.py from an interactive session. (expires 2026-07-15)
