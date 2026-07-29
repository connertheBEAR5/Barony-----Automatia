# Barony Automatia

Barony Automatia is a custom Barony source-code project focused on expanding the map editor, persistent-world support, custom map travel, dialogue and quest tools, controller usability, and additional gameplay systems.

## Main Features

- Up to 32 map layers
- New layered-map format with compatibility for older maps
- Two-way custom exits with persistent destination IDs
- Persistent entity IDs generated on map save
- Session-based persistent world changes across map travel
- Persistent destructible decorations and mechanisms
- Custom NPC dialogue and quest systems
- Branching choices, requirements, and NPC actions
- Enemy squads and named elite enemies
- Configurable map fog
- Improved editor 3D camera controls and decoration previews
- Progressive nine-slot magic hotbar
- Controller and keyboard/mouse magic-hotbar support
- Pit warning, voluntary falling, blind falling, and knockback falling
- Held-orb lighting
- Custom multiplayer travel behavior

## Current Development Status

Most major editor, travel, persistence, dialogue, and gameplay foundations are implemented.

Still experimental or incomplete:

- Flame-specific editor culling remains unfinished
- Temporary diagnostic logging still needs cleanup
- Cross-session and cross-map global quest checks require a dedicated save system
- Wider regression testing is still required

## Building

Typical Linux build commands:

```bash
cmake --build build --target barony -j1
cmake --build build --target editor -j1
```

## Documentation

A detailed player and map-maker guide is included at:

```text
helpful stuff/Barony Automatia Complete Features Guide.txt
```

It explains how the custom features work, how they are used, current limitations, multiplayer notes, and development status without describing the C++ implementation.

## Compatibility Notes

- New maps default to the expanded layered format.
- Older Barony maps remain loadable and default to three layers.
- Some custom properties require newer Automatia map versions.
- Automatia-specific maps may not work correctly in an unmodified Barony build.
- Persistent-world state is primarily session-based unless a feature explicitly saves into a normal game save.
- Custom multiplayer features should be tested with both host and client.

## License

Barony Automatia is based on the Barony source project. And is open sourced. 
