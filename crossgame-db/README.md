# CrossGameDB

`community-v1.json` stores versioned cross-title profiling/compatibility knowledge.
The database is data, not executable patch logic.

The native runtime does not execute Python. Current CrossGameDB analysis/training workflows may invoke `tools/crossgame_db.py`; generated decisions are consumed through versioned data/manifests rather than runtime source patch injection.

Keep entries architecture-neutral where possible and do not add copyrighted game/SDK payloads to the database.
