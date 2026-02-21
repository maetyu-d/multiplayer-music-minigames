# Multiplayer Music (Couch Co-op) - C++ MVP

Realtime local multiplayer music game with two coupled grids:

- `Macro Grid` (8x8): harmony/chord progression by bar.
- `Micro Grid` (16x8): rhythmic and melodic triggers by 16th-note step.

## Build

Requirements:

- CMake 3.16+
- SDL2 development package
- JUCE source checkout (default expected path: `/Users/md/JUCE`)
- C++17 compiler

```bash
cmake -S . -B build
cmake --build build
```

Run:

```bash
./build/multiplayer_music
```

If JUCE is in a different location:

```bash
cmake -S . -B build -DJUCE_DIR=/absolute/path/to/JUCE
cmake --build build
```

## Controls

Player 1 (Macro):

- `W/S` move macro row cursor up/down
- `A/D` move macro column cursor left/right
- `Space` commit chord in selected macro cell

Player 2 (Micro):

- `Arrow keys` move micro cursor (row/column)
- `Enter` toggle micro cell
- `Backspace` clear entire micro grid

Global:

- `P` pause
- `Tab` preview tone
- `T` toggle tutorial overlay
- `Esc` quit

## Tutorial mode

- Tutorial mode starts enabled and guides both players through core actions.
- It advances automatically as players move/place on both grids.

## Multi-game rotation

- Game start order is randomized between three games each run.
- After each game ends, it shows a 5-second test-card cooldown, then loads an unplayed game.
- The app exits after all three games have been played once.
- Duel controls (beat-locked actions):
  - `P1`: `A/D` move, `W` attack, `S` block
  - `P2`: `Left/Right` move, `Up` attack, `Down` block
- Rail Signal Rush controls:
  - `P1 macro route`: `A/D` select junction, `W` set route up, `S` set straight, `X` set route down
  - `P2 micro toggles`: `Left/Right` select junction, `Enter` toggle flip, `Up` force flip on, `Down` force flip off

## Notes

- Round length is 3 minutes.
- BPM ramps from 120 to 140 over time.
- Score combines timing, harmony, groove, and rotating objective checks.
