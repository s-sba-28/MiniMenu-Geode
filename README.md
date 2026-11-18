# MiniMenu

MiniMenu is a Geometry Dash utility mod for Geode. It provides an in-game
settings panel with noclip, hitbox and trajectory displays, a speed control,
custom death delay, and an input recorder/replayer.

> Noclip, speed changes, and replay playback put the level in test mode. They
> are intended for practice and do not submit a normal run.

## Compatibility

- Geode `6.0.0-alpha.2`
- Geometry Dash `2.2091`
- Windows 64-bit and Android 64-bit builds are configured in GitHub Actions.

## Using the mod

- On Windows, press **Tab** to open or close the panel. On Android, use the
  floating **MM** button in menus, pause screens, and level-complete screens.
- In **Main**, toggle the utilities or adjust speed and death delay.
- In **Replay**, set **Bot** to **Record**, play the level, then choose **Save**
  and name the recording.
- Select a saved replay, set **Bot** to **Replay**, and restart the level to
  play it back. Replay files use the `.mmr` extension.
- **Import** shows the replay folder. Copy `.mmr` files there and tap
  **Refresh**.

Replays are stored in the mod's Geode save directory, under `replays`. Do not
edit replay files while they are selected or being played.

## Building

Install the Geode SDK for the target platform, set `GEODE_SDK` to its folder,
then configure and build with CMake:

```powershell
cmake -B build -S . -G Ninja
cmake --build build
```

The GitHub Actions workflow builds Windows and Android 64-bit packages against
the Geode version declared in `mod.json`.

## Adding a logo

Add your square PNG artwork as `logo.png` in the repository root and commit it
with the mod. Geode Index uses this repository logo for the mod listing; it
does not automatically replace the in-game **MM** button. To show custom
artwork inside the game, add the image as a mod resource and load it in
`src/overlay.cpp`.
