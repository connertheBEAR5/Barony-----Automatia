#!/usr/bin/env python3
"""Emit an apply_patch patch for the maintained Illusion spell audit."""

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAGIC_HEADER = (ROOT / "src/magic/magic.hpp").read_text()
PRE_ILLUSION_HEADER = MAGIC_HEADER.split("static const int NUM_SPELLS")[0]
SPELL_CONSTANTS = {
    int(value): name
    for name, value in re.findall(
        r"static const int (SPELL_[A-Z0-9_]+)\s*=\s*(\d+)\s*;",
        PRE_ILLUSION_HEADER,
    )
}
ITEMS_JSON = json.loads((ROOT / "build/items/items.json").read_text())
SPELL_JSON = {
    int(value["spell_id"]): (name, value)
    for name, value in ITEMS_JSON["spells"].items()
}


def tier(difficulty):
    if difficulty < 20:
        return "Novice"
    if difficulty < 40:
        return "Basic"
    if difficulty < 60:
        return "Skilled"
    if difficulty < 80:
        return "Expert"
    if difficulty < 100:
        return "Master"
    return "Legendary"


def row_for(spell_id):
    constant = SPELL_CONSTANTS.get(spell_id, f"SPELL_ID_{spell_id}")
    if spell_id in SPELL_JSON:
        internal, value = SPELL_JSON[spell_id]
        display = value.get(
            "spell_name", internal.replace("spell_", "").replace("_", " ").title()
        )
        school = value.get("school", "internal/NPC")
        spell_type = value.get("spell_type", "DEFAULT")
        mana = value.get("mana", 1)
        sustain = value.get("sustain_mana", 0)
        difficulty = value.get("difficulty", 100)
        duration = value.get("duration", 0)
        damage = value.get("damage", 0)
        damage_mult = value.get("damage_mult", 1.0)
        damage2 = value.get("damage2", 0)
        duration_mult = value.get("duration_mult", 1.0)
        distance = value.get("distance", 0)
        radius = value.get("radius", 0)
        cast_time = value.get("cast_time", 1.0)
        tags = set(value.get("effect_tags", []))
        book = value.get("spellbook_internal_name", "")
        ordinary = value.get("drop_table", -1) >= 0
        learnable = bool(book)
        projectile = "PROJECTILE" in spell_type
        reflect = "yes" if projectile else "conditional"
        absorb = "yes" if projectile else "conditional"
        excluded = {26, 44, 100, 155, 156}
        if projectile and learnable and spell_id not in excluded:
            store = "yes"
        elif spell_type in {"AREA", "TOUCH_ENEMY", "TOUCH_ENTITY", "DIVINE_TARGET"}:
            store = "conditional"
        else:
            store = "no"
        snapshot = (
            "yes"
            if store == "yes" and ordinary
            else ("conditional" if store != "no" else "no")
        )
        recast = snapshot
        ai_safe = "yes" if projectile and ordinary else "conditional"
        overlap = []
        lower = internal.lower()
        if any(term in lower for term in ("reflect", "absorb", "seize")):
            overlap.append("Mirror Reflect/Store Magic")
        if any(term in lower for term in ("confuse", "fear", "charm", "dominate")):
            overlap.append("Paranoia/Mimic")
        if any(term in lower for term in ("teleport", "dash", "jump", "lift", "windgate")):
            overlap.append("Shadow Step/Path")
        if any(
            term in lower
            for term in ("hologram", "invisibility", "polymorph", "form", "illusion", "shade")
        ):
            overlap.append("Other/Copy/Mimic")
        if any(
            term in lower
            for term in ("wall", "path", "tunnel", "earth", "levitation", "flutter")
        ):
            overlap.append("wall/path/vertical")
        overlap_text = ", ".join(overlap) or "none direct"
        effect = (
            f"dmg {damage} x{damage_mult:g}"
            if damage or "DAMAGE" in tags
            else ("status/utility scaling" if tags else "implementation-defined")
        )
        if damage2:
            effect += f"; secondary {damage2}"
        duration_text = (
            f"{duration} ticks x{duration_mult:g}"
            if duration
            else "instant/implementation-defined"
        )
        generation = (
            (f"book {book}" if book else "no ordinary book")
            + ("; generated" if ordinary else "; hidden/internal")
        )
        ai = "explicit tables only"
        friendly_fire = "native delivery checks"
        ui = (
            "native spell/hotbar/tooltip"
            if ordinary
            else "hidden/internal presentation"
        )
    else:
        internal = constant.lower()
        display = (
            "None"
            if spell_id == 0
            else constant.removeprefix("SPELL_").replace("_", " ").title()
        )
        school = "internal/NPC"
        spell_type = "implementation-defined"
        mana = "source default"
        sustain = 0
        difficulty = 100
        effect = "source-specific"
        duration_text = "source-specific"
        distance = 0
        radius = 0
        cast_time = "source-specific"
        reflect = "conditional"
        absorb = "conditional"
        store = "no"
        snapshot = "no"
        recast = "no"
        ai_safe = "conditional"
        generation = "no ordinary book; internal registry"
        ai = "source-specific"
        friendly_fire = "source-specific checks"
        ui = "hidden/internal presentation"
        overlap_text = "none direct"
    target = spell_type.replace("_", " ").lower()
    spatial = "Auth server; persist known-only; PZ exact; MI active"
    matrix = (
        f"Store {store}; Reflect {reflect}; MirrorReflect {reflect}; "
        f"Absorb {absorb}; snapshot {snapshot}; recast {recast}; AI-safe {ai_safe}"
    )
    return (
        f"| {spell_id} `{constant}` | {display} / `{internal}` | "
        f"{school}; {spell_type}; MP {mana}; sustain {sustain}; "
        f"diff {difficulty} ({tier(difficulty)}) | {effect}; duration {duration_text}; "
        f"cast {cast_time}; range {distance}; radius {radius}; target {target} | "
        f"R {reflect}; A {absorb}; FF {friendly_fire} | Player registry; AI {ai}; "
        f"{generation} | {spatial} | {ui}; S.A.M. uses stable definition and runtime "
        f"mapping; overlap {overlap_text}; reuse cast/effect/target helpers | "
        f"{matrix} |"
    )


HEADER = """Illusion Magic Spell Comparison and Compatibility Audit
========================================================

Status: implementation-complete audit. Existing IDs 0..224 remain frozen;
Illusion occupies the new append-only range 225..237.
Authoritative inputs: current `src/magic/magic.hpp`, `src/magic/setupSpells.cpp`,
`src/magic/castSpell.cpp`, `src/magic/actmagic.cpp`, `src/magic/spell.cpp`, and
the current runtime `build/items/items.json`.

Method and invariants
---------------------

This table enumerates every pre-Illusion numeric spell slot, 0 through 224.
JSON-expanded values are recorded where the current data supplies them; entries
without expanded JSON are marked implementation-defined rather than guessed.
All existing spell IDs are frozen. "Conditional" in defense columns means the
delivery/effect implementation must be inspected at impact time (area, touch,
self, traps, vertical projectiles, or bespoke effects cannot safely be treated
as ordinary missiles).

The shared compatibility baseline for every row is: the server owns gameplay
mutation; known player spells use the normal character save; transient
projectiles/effects are not independently persisted; targeting must remain in
the active MapInstance and exact `playableFloor`; `authoredMapLayer`, local
`Entity::z`, and same X/Y are not ownership or targeting shortcuts. S.A.M.
definitions retain stable IDs while runtime IDs remain session-local.

Columns
-------

- Identity: numeric ID and source constant.
- Definition/balance: display/internal name, school, delivery, mana, sustain,
  learning difficulty and real proficiency tier.
- Effect/target: damage/effect scaling, duration, cast time, range, radius and
  target style.
- Defenses: ordinary reflection, absorption and friendly-fire behavior.
- Availability: player, AI, spellbook/generation path.
- Authority/persistence/spatial: networking, save and Playable-Z/MapInstance.
- UI/S.A.M./overlap/helper: current presentation, extension compatibility,
  proposed Illusion overlap and reusable infrastructure.
- Eligibility matrix: Store Magic, ordinary reflection, Mirror Reflect,
  absorption, pointer-free snapshot, safe recast and AI-cast safety.

Every current spell
-------------------

| Identity | Definition and balance | Effect and target | Defenses | Availability | Authority, persistence and spatial | UI, S.A.M., overlap and helpers | Store Magic eligibility matrix |
|---|---|---|---|---|---|---|---|
"""

FOOTER = """

Cross-system conclusions before implementation
----------------------------------------------

- Reflection and Absorption already meet in the projectile-impact pipeline.
  Store Magic and Mirror Reflect must enter that single decision point, before
  ordinary reflection/absorption, and consume exactly one outcome.
- Hologram supplies a no-drop, no-XP, non-recruitable ephemeral actor seam for
  Mirror Copy and Shadow Step; polymorph/invisibility must not be reused as
  identity replacement.
- Confusion supplies temporary target-perception behavior for Paranoia without
  persisting faction changes.
- Teleport validation and Playable-Z-aware map access are reusable for Shadow
  Step, but the destination must remain exactly four tiles backward or fail.
- Navigation illusions need actor-aware, bounded overlays keyed by active
  MapInstance plus exact playableFloor; map tiles themselves must not change.
- Known spells can use the existing save path. Illusion entities, fake loot,
  navigation overlays and the chosen runtime-only Stored Magic Vault must not.
- A fourth fixed proficiency array slot would change save and packet contracts.
  Illusion therefore uses an explicit effective-school proficiency derived from
  the strongest current magic proficiency while each spell retains Mysticism
  as its compatibility skill for legacy spellbook/fumble infrastructure. The UI
  labels it Illusion and exposes the derived tier.

Final Illusion values
---------------------

See the maintained audit for the final 225..237 balance table and Store Magic
formula/policy. The generator intentionally owns the 0..224 baseline table;
after regeneration, preserve the maintained final implementation section.
"""


CONTENT = HEADER + "\n".join(row_for(spell_id) for spell_id in range(225)) + FOOTER
print("*** Begin Patch")
print("*** Add File: helpful stuff/Illusion Magic Spell Comparison and Compatibility Audit.txt")
for output_line in CONTENT.splitlines():
    print("+" + output_line)
print("*** End Patch")
