# Evidence format

Canonical evidence uses `evidence-schema-v1.json`. Transient media, packet payloads, account identifiers, passphrases and unbounded logs are excluded.

Each bundle binds an observation to a commit, binary hash, test UUID, UTC date, operator type, exact environment, results, hashed artifacts and limitations. `human` is required for protected-game gameplay and matchmaking. `automated-transport-only` is limited to capture, codec, transport, resilience and benign owned-window tests.

Evidence validators check structure and security invariants; they do not convert an observation into vendor certification.
