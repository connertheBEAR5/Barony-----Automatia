# Barony Automatia

Barony Automatia is a heavily extended Barony source build focused on persistent worlds, nonlinear campaigns, taller maps, multiplayer world instances, custom dialogue and quests, expanded editor tools, dedicated-server support, and new magic systems.

This document summarizes the features currently present in this source tree. Features marked **experimental** are implemented but still need broader multiplayer or cross-platform acceptance testing.

## Build profile

The primary supported development configuration is:

- Linux with GCC and CMake
- FMOD or OpenAL selected explicitly at configure time
- Both the game and editor built from the same source tree

Use separate build directories when switching audio backends so dependency
detection and generated configuration cannot leak between them:

```bash
cd /home/conner/Barony-----Automatia
cmake -S . -B build-fmod -DBARONY_AUDIO_BACKEND=FMOD
cmake --build build-fmod -j5

cmake -S . -B build-openal -DBARONY_AUDIO_BACKEND=OPENAL
cmake --build build-openal -j5
```

`build-super-posix` is the current configured Steamworks-enabled, FMOD,
15-player development tree. The five-job limit is intentional on the current
development host to avoid CPU/RAM instability from an unrestricted parallel
build.

`BARONY_AUDIO_BACKEND` accepts `FMOD`, `OPENAL`, `NONE`, or `AUTO`. `AUTO`
keeps the legacy `FMOD_ENABLED` and `OPENAL_ENABLED` switches working for old
scripts; new configurations should use the explicit backend name. OpenAL uses
libogg/libvorbis by default, or Tremor when `TREMOR_ENABLED=ON`.

## Expanded maps and rendering

### 32-layer maps

Automatia maps can use up to 32 tile layers instead of the original three. This supports towers, bridges, overhead structures, deep pits, stacked rooms, and more detailed vertical spaces.

- Newer map files store their layer count.
- Legacy Barony maps continue to load with their original layout.
- The editor includes vertical 3D-camera movement for inspecting tall maps.
- Hybrid visibility preserves the original expanded 2D behavior on layers 0–2 while layers 3–31 use exact layer-aware visibility masks.
- Low walls no longer erase or incorrectly hide taller structures above them.
- Stale visibility and voxel-chunk state is cleared safely during restart and map transitions.

### Playable-Z coordinate model

The current playable-Z implementation keeps physical height separate from
gameplay membership:

```text
authoredMapLayer = physical/authored structural layer, 0..31
playableFloor    = gameplay and spatial-simulation membership
Entity::z        = local model/animation elevation
worldZ           = Entity::z + mapLayerWorldZ(authoredMapLayer)
mapLayerWorldZ(N)= -16 * N
```

Status snapshot: the current local-Z/structural-layer implementation, legacy
ceiling-model compatibility, upper/lower rendered-stack visibility, structural
lighting, lower-floor landing, HUD/camera attachments, ELYR/PZLV persistence,
and deterministic regressions compile in `build-super-posix`. Z4A adds a
MapInstance-local vertical transition graph derived from existing ZLDR/ZTRN
metadata; dropped items crossing an authored-floor boundary now continue onto
the next lower playable floor while retaining local Z and identity. The latest automated run
passed all 13 CTest targets and the Stage 4D/Z3 executable characterization.
Graphical acceptance items are listed below and are not claimed by those
deterministic tests.

Layer-authored floors form one vertically stacked rendered world. Geometry,
sprites, flames, and visual children on both higher and lower floors remain
eligible for rendering; depth and authored geometry provide occlusion. Collision,
interaction, mechanisms, and entity simulation remain isolated by
`playableFloor`.

Dynamic authored-stack lights sample and write the structural light volume at
their physical layer. Light may spill into adjacent structural layers with
three-dimensional attenuation when the crossed authored floor/ceiling planes
are open; opaque geometry blocks that transport. This is not a fullbright or
lower-floor-lightmap-copy workaround.

Legacy one-floor maps, including the animated main-menu maps, retain their
historical nonzero-light-layer wall masks. The playable-stack rule that maps a
light slice to the obstacle layer above it is enabled only when the map actually
contains a derived authored-floor stack, so an ordinary legacy ceiling cannot
black out a wall band. On a real stack, that obstacle lookup uses the absolute
authored layer directly and does not apply `playableFloor` a second time.

Legacy entity presentation also retains its original shader convention: models
and sprites in a one-floor map sample slice 0 of that map's light texture. This
matters for editor sprite 119, the ceiling-tile model, whose historical runtime
height is local Z -24. Treating that local height as light slice 2 made the
Labyrinth ceiling models (1219/1220) in `mainmenu3.lmp` appear black even though
nearby slice-0 torches lit the room. This compatibility rule is selected from
explicit authored-stack metadata, not by guessing from negative Z. Physical
light-source and CPU height lookups remain separate from this visual rule.

Entity sampling within an authored stack is anchored to
`authoredMapLayer`. Ordinary local model offsets do not select a neighboring
light slice merely because they cross the old nearest-layer midpoint: for
example, a lever base at local Z 7.5 and its handle at local Z 8.5 both sample
the lever's authored layer. A complete 16-unit local traversal can still move
the sample to an adjacent structural slice. Legacy one-floor maps keep their
existing source/CPU world-Z-to-light-layer conversion while their GPU entity
presentation uses the historical base slice.

First-person casting hands and the inventory character preview keep their
camera-local presentation while selecting the player's current structural
slice. Sustained Light and Deep Shade orbs are player-bound spatial attachments:
an existing PZTR stair transition moves their authored/playable context and
recreates their emitted light on the destination slice. Ordinary fired
projectiles deliberately remain on their source floor.

Project Spirit is also structural-context aware. A local ghost inherits the
caster directly; a remote client's legacy `GHOS` request is resolved from the
server-authoritative living player entity without adding or trusting a client
floor field. The spirit cosmetic camera retains that inherited structural
offset, so casting upstairs does not present the spirit at layer zero.

The post-death camera follows the same rule. It keeps its orbit height as local
camera Z while adding the followed player or ghost's authored structural offset.
When its target disappears, the independent camera retains the structural
context captured at death; the client automaton-death path inherits that context
at creation instead of constructing a layer-zero camera. The ordinary
multiplayer death notification does the same using the client's authoritative
player or ghost entity; no new packet field is required.
The focused upper-floor death-camera behavior has been manually verified; a
broader remote spectator-target cycle remains part of multiplayer acceptance.

Walking off an upper ledge searches downward for the nearest valid lower
authored surface before the legacy void path runs. The authoritative transition
keeps local Z, moves the full collision footprint inward if the ledge coordinate
would overlap a lower-floor wall, and applies one existing fall-damage unit per
structural floor crossed. Levitation continues to prevent the ordinary fall.

### Configurable map fog

Maps can define fog distance, density, color, and enabled state for caves, outdoor maps, magical areas, and atmospheric campaign environments.

### Editor previews and lighting

Selected decoration, collider, stair, and ceiling-tile entities receive useful
3D model previews. Ceiling sprite 119 is previewed with the same default model
621 (or configured model), local Z -24, and quarter-turn direction used at
runtime; `worldRenderZ()` adds a modern authored layer exactly once. Temporary
editor lighting illuminates active layers without being saved into the map, so
Zed is intentionally a placement/readability preview rather than a pixel-exact
runtime lighting comparison.

## Nonlinear travel and divergent routes

### Linked custom exits

Custom exits can identify themselves and a destination exit, allowing multiple entrances, two-way travel, and consistent arrival positions without duplicating an entire map for every entrance.

### Reverse ladders

Editor sprite 42 is a dedicated reverse ladder that travels to the previous visited floor. It uses a ceiling-mounted ladder-hole visual and does not replace the behavior of normal ladders, secret ladders, or decorative ladder holes.

### Per-player visited-route history

Each player keeps their own route stack. A reverse ladder follows the route that player actually traveled instead of assuming a fixed dungeon sequence. Skipped branches, including Hell, are not inserted into the return path.

Each saved return point records its map-instance identity, `playableFloor`,
`authoredMapLayer`, local X/Y/Z, and rotation. These fields are restored as
separate axes; a local Z value is never used to infer the structural layer.

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

Spatial persistence is scoped by map instance and floor. A persistent dynamic
entity records `playable_floor`, `authored_map_layer`, and local `position[2]`
separately. A tile override is identified by map instance plus playable floor,
X, Y, and geometry layer, so two modified objects or tiles at the same X/Y on
different floors do not alias.

Older records that predate `authored_map_layer` remain readable. Migration uses
explicit derived-authored-stack metadata to recover the structural layer; an
explicit legacy `FLOR` floor remains structurally local to layer zero. The
loader does not infer current structural state from a numerical local-Z guess.

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
- Character placement and divergent return records preserve map instance,
  playable floor, authored layer, and local Z independently.
- Schema validation rejects out-of-range structural layers, unknown schema-3
  floor references, non-finite local positions, and duplicate
  floor/X/Y/geometry-layer tile keys.

## Custom items and S.A.M. compatibility

Automatia preserves arbitrary nonnegative runtime item IDs rather than assuming every item is below the original `NUMITEMS` limit.

- Optional stable IDs identify S.A.M. items independently of their current-session numeric mapping.
- Stable IDs are authoritative when available.
- Unknown custom-item records remain in JSON for later recovery.
- Monster inventory and equipment templates preserve status, beatitude, quantity, identification, chance, category, slot information, and source metadata.
- Zed's concrete item fields store the historical `runtime item ID + 2` value;
  map metadata records the S.A.M. stable ID so reload can remap it safely.
- The loaded S.A.M. catalog and deterministic room-provider selection contribute
  to the authenticated `SAMF` fingerprint used by lobby and late-join checks.
- S.A.M. 2.1 room hooks append deterministic prefab candidates to the existing
  procedural room pool. They do not replace authored Room Groups.
- Current S.A.M. 2.1 contributes no Text Source commands. Automatia's unified
  Text Source extension registry is the supported future adapter seam.

## Authored Mini Mimic NPCs

Zed exposes the existing runtime Mini Mimic as a searchable creature entry; it
does not duplicate the monster implementation. Authored properties include an
optional display name, hostile/passive/friendly disposition, independent
recruitability, optional custom dialogue resource, and a two-state appearance
selector. Dialogue and recruitment may be enabled together.

The default `Baby` appearance keeps the dedicated Mini Mimic trunk and lid
(models 1794/1795), relines their inward-facing mouth surfaces, and adds
downsampled flesh/gum/teeth inside existing cavity space. Its exterior
silhouette and boundary shell cells remain unchanged. Old maps therefore remain
Baby by default. `Scaled Mimic` instead reuses the complete regular
Mimic trunk/lid (1247/1248), scaled independently on X/Y/Z to fit the Mini
Mimic footprint. Both appearances use a calm idle: a stationary, non-attacking
Mini Mimic settles instead of hopping in place and breathes through a small lid
motion. Movement, pursuit, and attack animations remain the shared Mimic
behavior.

The polygon-model cache includes a fingerprint of the loaded voxel dimensions,
occupancy, and palettes. Changing either Mini Mimic slab therefore invalidates
an older `~/.barony/models.cache` automatically instead of silently rendering
stale geometry. The first launch after a voxel update rebuilds that cache;
later launches reuse it normally.

Mini Mimics reuse the normal monster combat, faction, follower ownership, HUD,
networking, dialogue/quest, persistence, and S.A.M. item identity paths. Their
persistent records keep MapInstance, playable floor, authored layer, local Z,
durable owner identity, dialogue configuration, inventory/equipment, and monster
state without Mini-Mimic-specific save formats. Appearance also travels through
the existing monster stat/bodypart replication paths; no new packet layout or
second monster implementation was added. The deterministic asset generator is
`tools/generate_minimimic_interiors.py`.

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

### Player, Party, and World quests

Schema-2 quests select true server-authoritative ownership with `quest.scope`:

- `player` keeps private state on the durable player identity.
- `party` shares one state through the server's persistent PartyID.
- `world` shares one state through the persistent world save, across map
  instances and playable floors.

Schema-1 Party/World values retain their historical Personal fallback until an
author explicitly upgrades the file in the remastered editor. Player-affecting
rewards still target the actor; shared quest ownership does not multiply them.

The editor's tutorial library includes scoped multiplayer examples, multi-stage
AND branches, floor-aware markers, and Defeat ID objectives. For monster death
objectives, set the same monster-property **Defeat ID** (Squad Defeat ID) on the
targets and use that number as the objective's `defeat_id`. The ordinary Squad
ID controls AI coordination and does not advance the quest counter.

Quest giver and objective markers can be typed manually or picked by temporarily
returning to the map. Each marker can be restricted to one `playable_floor` or
shown through the whole vertical column. The Advanced JSON tab is a multiline
caret editor; **Apply** validates in memory and **Apply & Save** writes the file
atomically.

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
- Searchable authored Mini Mimics with disposition, recruitment, dialogue,
  display-name, and Baby/Scaled Mimic appearance properties
- Persistent named Room Groups: multi-layer X/Y/layer cuboids supporting
  tile-only, sprite-only, or combined copy/paste, including air/empty space
- Room Group create/update/select/copy/paste/delete workflows integrated with
  undo/redo and V4.10 map save/load
- Map ambience metadata editing with MapInstance-aware runtime activation
- A Text Source Script Tester and searchable Library backed by the same native
  and extension registry used for runtime parsing

Room Groups keep `authoredMapLayer` as physical structure and preserve relative
layer offsets; they never translate layers into `Entity::z`. Sprite properties,
persistent IDs, Mini Mimic/Text Source data, and S.A.M. stable metadata travel
through the generic entity copy/save paths. Graphical editor acceptance is still
required for these workflows even where characterization tests pass.

## Multiplayer and followers

- The server owns authoritative world, quest, merchant, and persistence state.
- Followers retain ownership and equipment across divergent generated-floor transitions.
- Recruited followers can be removed when their owner disconnects and restored with ownership on reconnect.
- The follower HUD is rebuilt from the authoritative recruited-follower list after restoration.
- Map travel, persistent mechanisms, minimaps, dialogue, quests, shops, and item changes include multiplayer synchronization paths.
- The persistent party backend keeps durable Party IDs, member identity, leader
  and invitation state, scoped chat/UI data, and 15-player bounds separate from
  MapInstance membership.
- Contained two-client process tests cover divergent MapInstances, scoped party
  behavior, `SAMF` delivery, save/shutdown/restart, and Party ID hydration. This
  is not a substitute for the real graphical-client acceptance run.

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
--character-save=local
--character-save=steam
```

Implemented server features include:

- Hidden-window compatibility startup
- Direct LAN listener when explicitly requested
- Timed or immediate autostart
- Numeric server save slots
- Timed autosave and graceful save-on-shutdown
- Terminal commands such as `help`, `status`, `start`, `save`, and `shutdown`
- Explicit failure for unsupported security modes rather than pretending a server is protected

Headless multiplayer saves use `savegames/host/`. The character save and its
Automatia world companion carry the same transaction ID; the companion is
validated and atomically replaced through a temporary file. A dedicated server
does not serialize its process-only slot 0 as a real player. In per-character
save modes, disconnected remote characters stay in their own character files
rather than being resurrected from a shared slot record.

Public lobby publication, password authentication, and a fully renderless
startup path remain unavailable. The explicit direct-LAN late-join path has
passed contained new-player and cross-map process probes, but still requires
normal graphical clients and a token-matched returning-character acceptance.

## Late join, reconnect, and character restoration

The source includes a bounded direct-LAN late-join snapshot protocol with transfer IDs, revision checks, chunk accounting, CRC validation, staged loading, spawn authorization, and a final client-ready barrier.

Character-save work restores authoritative player state including inventory, effects, followers, identity information, and saved placement. Reconnect and divergent-instance behavior should continue to receive real multi-client acceptance testing.

For derived authored-stack maps, current entity packets reconstruct the usual
`authoredMapLayer == playableFloor` context. The existing packet formats do not
encode an independent authored layer for a hypothetical dynamic network entity
whose structural layer intentionally differs from its gameplay floor; that
case remains a documented future protocol concern rather than being guessed
from local Z.

## Steam runtime and offline mode

This remains one Steamworks-enabled executable; `--nosteam` is a runtime local
mode, not a separate build and not an ownership emulator.

- Normal launch attempts `SteamAPI_Init()`. When Steam initializes, existing
  Steam matchmaking, authentication, friends, achievements, Workshop/cloud,
  and DLC entitlement checks remain available.
- If Steam is unavailable and initialization fails, startup continues with the
  same Steam-services-off/DLC-locked capability set as explicit `--nosteam`.
- `--nosteam` deliberately skips Steam initialization. Steam-only services are
  unavailable, and Steam DLC flags remain locked/fail-closed.
- No Steam authentication ticket, DLC entitlement, offline token, or ownership
  result is fabricated. The Steam build does not enable the legacy non-Steam
  `.key`-file DLC path and does not parse Steam's private cache files.
- To use legitimately owned Steam DLC without network access, start the Steam
  client in its supported Offline Mode and launch normally without `--nosteam`.
  Steamworks, not Automatia, remains responsible for interpreting Steam's
  cached ownership state.

Important limitation: because `--nosteam` intentionally provides no SteamAPI
ownership signal, it cannot itself prove ownership of the Steam base game. The
custom executable does not include Barony's proprietary game data, but anyone
requiring Steam ownership enforcement must distribute/use it only with a
Steam-initialized launch and must not describe `--nosteam` as ownership
verification. This is a technical statement, not legal advice; review the
Barony and Steam distribution terms before publishing binaries.

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

The current Z4D baseline passes `cmake --build build-super-posix -j6`, all 13
CTest targets, the Stage 4D/Z4D characterization, and `git diff --check`. It includes
characterization for Mini Mimic authoring, Room Groups, S.A.M. room generation,
Text Source scripts, party/backend/chat/UI behavior, and Playable-Z. The
following areas still require manual or broader platform acceptance:

- Real multi-client divergent-map and late-join sessions
- Returning-player reconnect while other players occupy different instances
- Windows headless and recovery acceptance
- Every spell's semantic use of generic damage and duration fields
- Grimoire scaling on unusual utility, summon, and mod-added spells
- Merchant unlock/purchase behavior during disconnects and simultaneous multiplayer interaction
- Full editor duplication and persistent-ID collision handling
- Real S.A.M. mods with large catalogs, custom items in upper-floor containers
  and monster equipment, and room-hook content on all peers
- Named Room Group graphical create/select/copy/paste/delete/save/reload,
  including air, multiple authored layers, and S.A.M. entities
- Mini Mimic dialogue plus recruitment, durable owner restoration, and
  per-player dialogue/quest behavior with real clients; visually inspect both
  Baby and Scaled Mimic modes, calm idle/movement transitions, and a remote
  late join observing the authored appearance
- MapInstance-specific ambience and same-instance Playable-Z stair audio
- Text Source Tester/Library GUI behavior and supported S.A.M.-item references
- Original-map regression testing with 32 layers and hybrid visibility
- Cross-floor light appearance and ledge landings on representative authored
  maps, including narrow openings, thick ceilings, and wall-adjacent pits
- Legacy ceiling sprite 119 in `mainmenu3.lmp` and an ordinary old dungeon:
  verify historical lighting, no duplicate/z-fighting face, and agreement with
  the Zed model preview. Also place the same model on modern authored layers 0,
  2, and 5.
- Divergent Paths x Playable Z x persistence with two real clients, including
  same-X/Y objects on different floors and a full dedicated-server restart
- Steam client Offline Mode entitlement behavior with each owned/unowned DLC
  combination; automated tests do not emulate Steam's client-side cache

The tree has a green automated stabilization baseline. The owner subsequently
accepted beginning Z4 despite the still-open real-client acceptance work. Z4A
is complete: it provides the map-local vertical transition graph,
reconstruction/query coverage, and lower-floor dropped-item integration. Z4B
is also complete as a generic query-only route planner that combines ordinary
floor-local A* paths with explicit Z4A edges. Z4C is complete for normal
player-owned followers: the server sends them along those local paths, applies
real same-MapInstance stair transitions, and then resumes ordinary following.
Z4D is complete for ordinary hostile player targets: a hostile that already
legitimately targeted a player can retain pursuit after the player changes
floors, but only through a valid same-MapInstance Z4A/B route. Cross-floor
acquisition, attacks, collision and equal-X/Y adjacency remain forbidden;
passive, dialogue, stationary and recruited NPC roles retain their existing
behavior. Real graphical multiplayer acceptance remains part of the manual
list above.

## Maintained project documentation

- `INSTALL.md` — dependencies, the primary `-j6` build, and Steam runtime modes.
- `helpful stuff/Barony Automatia Complete Features Guide.txt` — complete
  feature and implementation-status reference.
- `helpful stuff/Codex Project Progress and Continuation.txt` — chronological
  engineering record plus the current checkpoint and acceptance plan.
- `helpful stuff/Divergent Paths and Late Join Developer Guide.txt` — map
  instances, packet scope, late join, reconnect, and Playable-Z persistence.
- `helpful stuff/Headless Server Developer Guide.txt` — supported launch,
  administration, save/recovery, and fail-closed boundaries.
- `helpful stuff/Headless Remaining Work and Security Requirements.txt` — work
  that must still be completed or runtime-accepted before broader deployment.
- `helpful stuff/Custom Dialogue and Quest Editor Guide.txt` — authoritative
  dialogue/quest authoring and editor reference.
- `helpful stuff/Custom Dialogue JSON Guide and Project Reminders.txt` — compact
  historical quick reference with current-status corrections.
- `helpful stuff/Text Source @script Guide vs Runtime Audit.txt` — authoritative
  Text Source language, Tester/Library, verification depth, and S.A.M. boundary.
- `helpful stuff/SAM 2.1 Automatia Integration Compatibility and Edge Case Audit.txt`
  — S.A.M. 2.1 architecture, identity, hooks, persistence, and interoperability.
- `helpful stuff/Automatia Cross-Feature Compatibility and Z4 Readiness Audit.txt`
  — current cross-feature matrix and exact one-headless/two-client acceptance.
- `helpful%20stuff/Quest%20Journal%20Backend.txt` — journal backend contract and
  current UI/persistence status.
- `build-super-posix/Automatia_Z4D_AI_Catchup_2026-08-29.txt` — detailed
  implementation handoff through Z4D and the Mini Mimic appearance/idle pass.
- `build-super-posix/Automatia_Z4D_AI_Catchup_2026-08-29.zip` — a verified
  snapshot containing that handoff and every source, test, asset, generator,
  and maintained documentation file changed by the covered work.

## Licensing

Automatia remains based on the open-source Barony codebase. Preserve the original copyright and license notices in redistributed source and binaries, and review the repository's license files before distributing or selling a modified build.
