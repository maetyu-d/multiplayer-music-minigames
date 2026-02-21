# Multiplayer Music (Couch Co-op)

Realtime local multiplayer minigame collection with music-driven co-op gameplay.

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

## Global controls

- `P` pause
- `Esc` quit

Title screen:

- `Up/Down` or `W/S` move selection
- `1-7` jump to game
- `Enter` or `Space` start selected game

## Game types, aims, and controls

### 1) Grid Co-op

Aim:

- Build a coherent track by keeping macro harmony and micro rhythm aligned.
- Score from timing, harmonic fit, groove, combo, and objectives.

Controls:

- `P1 (Macro)`: `W/S` row, `A/D` column, `Space` place chord
- `P2 (Micro)`: `Arrow keys` move cursor, `Enter` toggle cell, `Backspace` clear micro grid
- Extra: `Tab` preview tone, `T` toggle tutorial overlay

### 2) Chord Duel Arena

Aim:

- Beat-locked duel. Win by reducing opponent HP before time or by having more HP at timeout.

Controls:

- `P1`: `A/D` move, `W` attack, `S` block
- `P2`: `Left/Right` move, `Up` attack, `Down` block

### 3) Rail Signal Rush

Aim:

- Keep train throughput high and collisions low as tempo rises.

Controls:

- `P1 (Micro)`: `Left/Right` select junction, `Up/Down` toggle flip/norm on tick
- `P2 (Macro)`: `A/D` select junction, `W` set up, `S` set straight

### 4) Signal Forge

Aim:

- Co-op circuit + tuning game: power the radio with tile rotation while tuning carrier amidst noise.

Controls:

- `P1 (Builder)`: `W/A/S/D` move tile cursor, `Space` rotate tile, `Backspace` regenerate circuit
- `P2 (Tuner)`: `Left/Right` tune cursor, `Enter` attempt lock on beat window

### 5) Strangelove

Aim:

- Prevent escalation and devastation by balancing doctrine and timely intercepts.

Controls:

- `P1 (Command)`: `A/D` select doctrine slot, `W/S` change doctrine level, `Backspace` clear doctrine plan
- `P2 (Defense)`: `Left/Right` select lane, `Enter` intercept on beat

### 6) Double Snake

Aim:

- Two snakes share one board and one food economy. Grow score without crashing.

Controls:

- `P1 snake`: `W/A/S/D`
- `P2 snake`: `Arrow keys`

### 7) LongJump Duet

Aim:

- Best-of-3 long jump duel: match left/right foot rhythm to build speed, set angle, jump at the line, win by distance.

Controls:

- `P1`: `A/D` left-right foot steps, `W/S` angle, `Space` jump
- `P2`: `Left/Right` left-right foot steps, `Up/Down` angle, `Enter` jump

## Game flow

- Choose the starting game on the title screen.
- After each game, a short cooldown plays, then a random unplayed game is selected.
- Session ends after all games have been played once.
