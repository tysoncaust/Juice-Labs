# Contributing

1. Preserve the security invariants in `ARCHITECTURE.md`.
2. Keep passive and interactive code paths physically separated.
3. Add deterministic tests and bounded evidence for behavior changes.
4. Never add game automation, process inspection, injection, hooks, overlays, bypass logic, macros, replay or programmable timing.
5. Do not commit secrets, captured media, packet payloads or account identifiers.
6. Update the compatibility registry only from an exact versioned observation.
7. Run the passive build, evidence validator and repository secret scan before submitting a change.
