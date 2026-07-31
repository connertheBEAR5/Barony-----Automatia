# SAM-1Q3B8 — Remote Equipment Stable-ID Synchronization

Replacement files for the Barony `maindev` worktree.

This cumulative package preserves the prior SAM-1Q3 changes and adds authoritative `stable_id` payloads to remote client equipment requests:

- `EQUI` weapon equipment
- `EQUS` shield or spellbook equipment
- `EQUM` armor, mask, cloak, amulet, ring, and other equipment slots

Vanilla packets remain 28 bytes. Registered S.A.M. items append a bounded null-terminated `stable_id` beginning at byte 28. The server resolves the identity through its own registry and rejects malformed, unavailable, or numeric-only custom identities.
