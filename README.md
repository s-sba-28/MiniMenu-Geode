# MiniMenu

<p align="center">
  <img src="./logo.png" alt="MiniMenu logo" width="180">
</p>

**MiniMenu** is a Geometry Dash practice and utility mod built with Geode. It adds a compact in-game panel for gameplay tools and input replay.

> [!IMPORTANT]
> Noclip, speed changes, and replay playback can place a level in test mode and prevent normal-run completion or stat submission. Use these features for practice only.

## Features

- **Noclip** - Ignore deaths and hazards.
- **Hitboxes** - Display collisions with outline, fill, or both.
- **Trajectory preview** - Predict the player's path for holding and releasing.
- **Speed control** - Adjust and quickly reset level speed.
- **Custom death delay** - Control the respawn timing.
- **Replay bot** - Record inputs, save recordings, and replay them later.

## Opening MiniMenu

| Platform | Open / close |
| --- | --- |
| Windows | Press **Tab** |
| Android | Tap the floating MiniMenu logo |

The Android button is square with a black background and the MiniMenu logo centered above it. Drag it to move it; its position is saved automatically.

## Replay guide

1. Open **Replay**.
2. Set **Bot** to **Record**.
3. Restart the level and play normally.
4. Choose **Save** and give the recording a name.
5. Set **Bot** to **Replay**, select the replay, and restart the level.

Replay files use the `.mmr` extension and are stored under `replays` in the mod's Geode save directory.

- **Import** opens the replay folder.
- **Refresh** detects newly added `.mmr` files.
- Do not edit replay files while they are selected or playing.

## Compatibility

- Geode `6.0.0-alpha.2`
- Geometry Dash `2.2091`
- Windows 64-bit
- Android 64-bit

## Building

Install the Geode SDK for the target platform and set `GEODE_SDK` to its directory:

```powershell
cmake -B build -S . -G Ninja
cmake --build build
```

GitHub Actions builds Windows and Android 64-bit packages against the Geode version declared in `mod.json`.
