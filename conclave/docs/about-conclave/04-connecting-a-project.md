# Connecting a project to Conclave

Conclave is cross-project: every project plugs in both to **teach** it (feed it the
project's knowledge) and to **use** it (escalate hard problems). All of a project's
state is namespaced by `--project <name>` under `data/projects/<name>/` and
`policies/<name>.json`.

## One-time setup of the Conclave install

1. `conclave.toml` defines the panel, judge, embedders, and cadences.
2. Copy `secret.example` to `secret` and put an OpenRouter API key in it. `secret` is
   gitignored and must stay untracked (`conclave doctor` enforces this).
3. Enable escalation by exporting `CONCLAVE_ENABLED=1` (the `ask` verb sets it for its
   own call; background jobs require it in the environment).
4. `conclave doctor --online` to confirm secret, state, storage, balance, and panel.

## The escalation ladder lives in the project, not here

The Claude → Codex → Conclave ladder is **project-side** — implemented in each
consuming project (e.g. under its own `.conclave/`). Conclave only provides the apex
rung and its job queue. A project escalates to Conclave when its own agent and reviewer
are both stuck, passing their prior analyses as `prior` so the judge can build on them.

## Workflow to connect a project

1. **Teach the corpus.** Point `teach` at the project's docs/specs/source so Conclave
   knows the project before any escalation:

   ```
   conclave teach --project myproj --source /path/to/myproj/docs
   ```

2. **Use it when stuck.** Synchronously:

   ```
   conclave ask --project myproj --mode stuck --target "<problem-signature>" \
     --source brief.md
   ```

   …or asynchronously with `dispatch`, then collect the result by job id.

3. **Feed the outcome back.** After applying the synthesis and learning whether it
   worked:

   ```
   conclave outcome --project myproj --id <job-id> --verdict resolved
   ```

   This closes the learning loop: panel/routing policies update and, on `resolved`, the
   synthesis becomes a retrievable lesson for next time. The outcome label can also be
   inferred automatically from the project's own build/test success signal.

## Budgets and safety, per project

- `max_calls` caps panel calls per project (tracked in the project's `budget.json`).
- `min_credits` is a hard USD floor; below it, escalation fails closed.
- Nothing runs unless `CONCLAVE_ENABLED=1` and `secret` is set, so an unconfigured or
  broke install simply returns a status and never blocks the caller.
