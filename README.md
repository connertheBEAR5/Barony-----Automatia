# HEADLESS-1A Dedicated Server Startup Skeleton

Install into the maindev worktree root:

```bash
cd ~/maindev
unzip -o ~/Downloads/HEADLESS-1A_dedicated_server_startup_skeleton.zip -d ~/maindev
```

Launch the first-stage headless mode with:

```bash
./barony --headless
```

Existing map and data arguments can be combined with it, for example:

```bash
./barony --headless -map=start
```

This first stage uses a hidden OpenGL window/context for compatibility with existing resource loading. It disables visible presentation, sound initialization, controller/haptic initialization, controller mapping loading, and splash music. It does not yet automatically create and configure a multiplayer server lobby.
