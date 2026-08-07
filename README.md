# Barony Automatia

Barony Automatia is a heavily extended Barony source build focused on persistent worlds, nonlinear campaigns, taller maps, multiplayer world instances, custom dialogue and quests, expanded editor tools, dedicated-server support, and new magic systems.

This document summarizes the features currently present in this source tree. Features marked **experimental** are implemented but still need broader multiplayer or cross-platform acceptance testing.

## Build profile

The primary supported development configuration is:

- Linux with GCC and CMake
- FMOD enabled
- OpenAL disabled unless a separate compatibility build is being tested
- Both the game and editor built from the same source tree

A typical existing-build command is:

```bash
cd /home/conner/Barony-----Automatia
cmake --build build -j"$(nproc)"
```

## Expanded maps and rendering

### 32-layer maps

Automatia maps can use up to 32 tile layers instead of the original three. This supports towers, bridges, overhead structures, deep pits, stacked rooms, and more detailed vertical spaces.

- Newer map files store their layer count.
- Legacy Barony maps continue to load with their original layout.
- The editor includes vertical 3D-camera movement for inspecting tall maps.
- Hybrid visibility preserves the original expanded 2D behavior on layers 0–2 while layers 3–31 use exact layer-aware visibility masks.
- Low walls no longer erase or incorrectly hide taller structures above them.
- Stale visibility and voxel-chunk state is cleared safely during restart and map transitions.

### Configurable map fog

Maps can define fog distance, density, color, and enabled state for caves, outdoor maps, magical areas, and atmospheric campaign environments.

### Editor previews and lighting

Selected decoration and collider entities receive useful 3D previews. Temporary editor lighting can illuminate active layers without being saved into the map.

## Nonlinear travel and divergent routes

### Linked custom exits

Custom exits can identify themselves and a destination exit, allowing multiple entrances, two-way travel, and consistent arrival positions without duplicating an entire map for every entrance.

### Reverse ladders

Editor sprite 42 is a dedicated reverse ladder that travels to the previous visited floor. It uses a ceiling-mounted ladder-hole visual and does not replace the behavior of normal ladders, secret ladders, or decorative ladder holes.

### Per-player visited-route history

Each player keeps their own route stack. A reverse ladder follows the route that player actually traveled instead of assuming a fixed dungeon sequence. Skipped branches, including Hell, are not inserted into the return path.

### Divergent player paths

Players can occupy different loaded map instances during the same server session.

- A transition moves only the activating player when the route supports individual travel.
- Recruited followers travel with their owner.
- Return positions and exit sides are stored per player.
- Exact return placement is restored when traveling backward.
- Loaded occupied instances continue limited authoritative simulation.
- Packet delivery and entity updates are filtered by map-instance occupancy.

This system is implemented and has passed targeted feature tests. Broad multi-client regression testing remains important because it changes many assumptions inherited from the original single-active-map architecture.

## Persistent world systems

### Stable persistent IDs

Authored and procedurally generated entities receive stable persistent identities. Generated entities use reserved deterministic ID ranges so their saved state can be matched after a map is regenerated.

### Persistent map state

Automatia records meaningful world changes instead of fully resetting a previously visited map. Supported state includes major portions of:

- Destructible decorations and collider objects
- Tables, chairs, fireplaces, arcane furniture, and other special furniture
- Doors and wall locks
- Power crystals and their circuit requirements
- Traps, fired boulder traps, and spawned boulders
- Signal timers, controllers, pressure plates, switches, and mechanisms
- Bells, sinks, and fountains
- Pedestals, sockets, receptors, and orb state
- Persistent tile changes, wall builders, and wall busters
- Chests, containers, and shop inventories
- Dynamic monsters, monster equipment, effects, and follower ownership
- Dropped items, gold, and stable custom-item metadata where supported

### Persistent minimap discovery

Minimap exploration is stored per player and per exact map instance.

- Discovery survives map travel, save/load, and reconnect.
- Generated floors use floor-specific identities such as `mine.lmp#level_1_regular`.
- The live minimap is cleared before restoring the exact destination floor, preventing discovery from leaking between floors with the same base filename.

### Save companion document

The project includes a versioned Automatia world-state companion save.

- The server is authoritative.
- Writes use a temporary file, validation, and atomic replacement.
- Unknown JSON fields are preserved for forward compatibility.
- Unresolved custom items retain their stable metadata instead of being silently deleted or clamped.
- Map-instance state, minimaps, mechanisms, dialogue, quests, and other persistent records can be restored with the normal character save.

## Custom items and S.A.M. compatibility

Automatia preserves arbitrary nonnegative runtime item IDs rather than assuming every item is below the original `NUMITEMS` limit.

- Optional stable IDs identify S.A.M. items independently of their current-session numeric mapping.
- Stable IDs are authoritative when available.
- Unknown custom-item records remain in JSON for later recovery.
- Monster inventory and equipment templates preserve status, beatitude, quantity, identification, chance, category, slot information, and source metadata.

## Dialogue and quests

### JSON NPC dialogue

NPCs can use external JSON dialogue with:

- Branching conversations
- Multiple player choices
- Item and world-state requirements
- Per-player seen-node memory
- Persistent choices and actions
- Mechanism activation and movement actions
- Quest acceptance, progression, and completion

### Per-player quests

Player-scoped quests are registered for joining players and handled authoritatively by the server. Late joiners can accept and update eligible quests without inheriting another player's private quest state.

### Quest journal

The quest journal provides quest lists, objective state, tracking information, controller navigation, camera/input locking, and inventory-safe close behavior.

## Editor extensions

Implemented editor work includes:

- Searchable sprite and tile palettes
- Searchable inventory-item selection
- Searchable large dropdowns and lists
- Expanded monster/NPC inventories beyond the original convenient slots
- Monster effect editing with strength, duration, and permanent effects
- 32-layer editing and vertical 3D navigation
- Custom exit, mechanism, persistence, dialogue, quest, fog, and other Automatia properties
- Named elites and enemy-squad organization

Some editor workflows still require broad regression testing, especially selection semantics, persistent-ID collisions after duplication, and large custom-item catalogs.

## Multiplayer and followers

- The server owns authoritative world, quest, merchant, and persistence state.
- Followers retain ownership and equipment across divergent generated-floor transitions.
- Recruited followers can be removed when their owner disconnects and restored with ownership on reconnect.
- The follower HUD is rebuilt from the authoritative recruited-follower list after restoration.
- Map travel, persistent mechanisms, minimaps, dialogue, quests, shops, and item changes include multiplayer synchronization paths.

## Headless server

Automatia includes a headless dedicated-server mode with Linux and Windows launch paths.

Common options include:

```text
--headless
--LAN
--port=<1-65535>
--bind=<address>
--server-name=<name>
--autostart
--autostart=<seconds>
--save=<slot>
--autosave=<seconds>
--late-join
--character_save=local
--character_save=steam
```

Implemented server features include:

- Hidden-window compatibility startup
- Direct LAN listener when explicitly requested
- Timed or immediate autostart
- Numeric server save slots
- Timed autosave and graceful save-on-shutdown
- Terminal commands such as `help`, `status`, `start`, `save`, and `shutdown`
- Explicit failure for unsupported security modes rather than pretending a server is protected

Public lobby publication, password authentication, and a fully renderless startup path remain unavailable. The direct-LAN late-join path is experimental and should be tested with multiple real clients before release use.

## Late join, reconnect, and character restoration

The source includes a bounded direct-LAN late-join snapshot protocol with transfer IDs, revision checks, chunk accounting, CRC validation, staged loading, spawn authorization, and a final client-ready barrier.

Character-save work restores authoritative player state including inventory, effects, followers, identity information, and saved placement. Reconnect and divergent-instance behavior should continue to receive real multi-client acceptance testing.

## Pit safety and movement

Sighted players receive warning feedback before voluntarily walking into a pit. Continued movement permits the fall after the warning period, while moving away resets it. Blindness, forced knockback, and levitation use their own intended rules.

## Magic hotbar and Magic Grimoire

### Nine-slot magic hotbar

Automatia provides a separate nine-slot magic hotbar that references real spell items in the player's inventory.

- Keyboard, mouse, and controller operation are supported.
- Assignments are invalidated when the required spell item is lost, dropped, or sold.
- The Magic Grimoire opens and uses this existing nine-slot bar while equipped in the offhand slot.

### Magic Grimoire acquisition

A run can legitimately contain up to two Magic Grimoires:

1. **Natural copy:** generated exactly once per run in an ordinary non-mimic dungeon chest.
2. **Mysterious Merchant copy:** unlocked by surrendering the Purple Orb to the Mysterious Merchant in Hamlet after defeating Baron Herx.

The Baron Herx exit pedestal now powers itself after Herx is defeated. The Purple Orb is no longer consumed to activate the portal, allowing it to be carried back to Hamlet.

The merchant transaction is server-authoritative and run-global:

- The Purple Orb is consumed by the exchange.
- Exactly one merchant Grimoire is added to stock.
- After the exchange, the merchant remains willing to open this special shop without requiring a blue, red, or green orb until the Grimoire is purchased.
- Purchasing the Grimoire does not consume another orb; the Purple Orb was already paid during the unlock exchange.
- The unlock survives map travel and save/reload.
- Once purchased, the merchant copy does not restock.
- Natural generation and merchant purchase use separate persistent flags.

### Grimoire spell potency

Only spells launched from the Grimoire's nine-slot hotbar while the Grimoire is equipped receive its bonuses.

For a spell, effective Grimoire skill is:

```text
Effective skill =
    100% of the spell's own magic-school proficiency
  + 50% of each other magic-school proficiency
```

There is no 100-point cap. With Sorcery, Mysticism, and Thaumaturgy all at 100, every spell receives an effective skill of 200.

```text
Potency G = 10%
          + 1% for every 10 effective skill
          + 5% per effective positive Grimoire blessing
```

Positive blessings count normally. A negative blessing counts as positive only for characters whose equipment rules invert beatitude.

Grimoire scaling:

| Spell result | Bonus |
|---|---:|
| Damage | 100% of G |
| Healing | 100% of G |
| Buff/debuff strength | 100% of G |
| Shield/barrier amount | 100% of G |
| Resource generation | 100% of G |
| Status duration | 50% of G |
| Range | 50% of G |
| Area coverage | 50% of G |
| Summon statistics | 50% of G |
| Summon lifetime | 50% of G |
| Cast time | Reduced using 50% of G |
| Supported refire/cooldown time | Reduced using 50% of G |

Area spells scale coverage rather than applying the full percentage directly to radius. Radius uses the square root of the area multiplier, preventing a modest potency bonus from producing a disproportionately large affected area.

Projectile count and summon count are not increased. Internal safety timers, network intervals, and hard control limits are not intentionally treated as spell power.

### Grimoire mana efficiency

```text
Mana reduction = effective skill / 4 percent
               + 5% per effective positive blessing
               capped at 50%
```

Initial spell costs use the reduced value with a minimum cost of one MP for a normally positive-cost spell. Sustained spells store the same Grimoire mana-efficiency information so recurring sustain costs can receive the discount when their normal payment is greater than one MP; extended effect duration also improves the efficiency of one-MP sustained effects.

## Known limitations and testing priorities

The following areas deserve continued testing before a public release:

- Real multi-client divergent-map and late-join sessions
- Returning-player reconnect while other players occupy different instances
- Windows headless and recovery acceptance
- Every spell's semantic use of generic damage and duration fields
- Grimoire scaling on unusual utility, summon, and mod-added spells
- Merchant unlock/purchase behavior during disconnects and simultaneous multiplayer interaction
- Full editor duplication and persistent-ID collision handling
- Large S.A.M. catalogs and unresolved custom-item round trips
- Original-map regression testing with 32 layers and hybrid visibility

## Licensing

Automatia remains based on the open-source Barony codebase. Preserve the original copyright and license notices in redistributed source and binaries, and review the repository's license files before distributing or selling a modified build.
