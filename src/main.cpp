#include <SDL.h>
#include "JuceAudioEngine.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
constexpr int kWindowW = 1360;
constexpr int kWindowH = 800;
constexpr int kMacroRows = 8;
constexpr int kMacroCols = 8;
constexpr int kMicroRows = 8;
constexpr int kMicroCols = 16;
constexpr float kRoundDurationSec = 180.0f;
constexpr float kStartBpm = 120.0f;
constexpr float kBpmRampEverySec = 45.0f;
constexpr float kBpmRampAmount = 5.0f;
constexpr float kBpmMax = 140.0f;
constexpr float kDuelOnBeatWindowSec = 0.09f;
constexpr float kRailOnTickWindowSec = 0.06f;
constexpr float kTau = 6.28318530718f;
constexpr int kRailArenaX = 120;
constexpr int kRailArenaY = 210;
constexpr int kRailArenaW = kWindowW - 240;
constexpr int kRailArenaH = 360;
constexpr int kSignalCols = 8;
constexpr int kSignalRows = 5;

struct RGB { uint8_t r, g, b; };

struct Particle {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float life = 0.0f;
    float maxLife = 0.0f;
    RGB color{255, 255, 255};
};

struct HitEvent {
    int lane = 0;
    int step = 0;
};

struct RailTrain {
    int cell = 0;
    int prevCell = 0;
    int lane = 0;
    int dir = 1;  // +1 right, -1 left
    float speed = 0.0f;
    int nextJunction = 0;
    RGB color{255, 255, 255};
};

void TriggerVoice(JuceAudioEngine& audio,
                  JuceAudioEngine::VoiceType type,
                  float freq,
                  float gain,
                  float decay,
                  float duration) {
    audio.triggerVoice(type, freq, gain, decay, duration);
}

struct Glyph { std::array<uint8_t, 7> rows{}; };
const std::unordered_map<char, Glyph> kFont = {
    {' ', {{0,0,0,0,0,0,0}}}, {':', {{0,4,0,0,4,0,0}}}, {'-', {{0,0,0,14,0,0,0}}},
    {'0', {{14,17,19,21,25,17,14}}}, {'1', {{4,12,4,4,4,4,14}}}, {'2', {{14,17,1,2,4,8,31}}},
    {'3', {{30,1,1,14,1,1,30}}}, {'4', {{2,6,10,18,31,2,2}}}, {'5', {{31,16,16,30,1,1,30}}},
    {'6', {{14,16,16,30,17,17,14}}}, {'7', {{31,1,2,4,8,8,8}}}, {'8', {{14,17,17,14,17,17,14}}},
    {'9', {{14,17,17,15,1,1,14}}}, {'A', {{14,17,17,31,17,17,17}}}, {'B', {{30,17,17,30,17,17,30}}},
    {'C', {{14,17,16,16,16,17,14}}}, {'D', {{30,17,17,17,17,17,30}}}, {'E', {{31,16,16,30,16,16,31}}},
    {'F', {{31,16,16,30,16,16,16}}}, {'G', {{14,17,16,23,17,17,14}}}, {'H', {{17,17,17,31,17,17,17}}},
    {'I', {{14,4,4,4,4,4,14}}}, {'J', {{1,1,1,1,17,17,14}}}, {'K', {{17,18,20,24,20,18,17}}},
    {'L', {{16,16,16,16,16,16,31}}}, {'M', {{17,27,21,21,17,17,17}}}, {'N', {{17,25,21,19,17,17,17}}},
    {'O', {{14,17,17,17,17,17,14}}}, {'P', {{30,17,17,30,16,16,16}}}, {'Q', {{14,17,17,17,21,18,13}}},
    {'R', {{30,17,17,30,20,18,17}}}, {'S', {{15,16,16,14,1,1,30}}}, {'T', {{31,4,4,4,4,4,4}}},
    {'U', {{17,17,17,17,17,17,14}}}, {'V', {{17,17,17,17,10,10,4}}}, {'W', {{17,17,17,21,21,21,10}}},
    {'X', {{17,10,4,4,4,10,17}}}, {'Y', {{17,10,4,4,4,4,4}}}, {'Z', {{31,1,2,4,8,16,31}}},
    {'(', {{4,8,16,16,16,8,4}}}, {')', {{4,2,1,1,1,2,4}}}, {'/', {{1,2,4,8,16,0,0}}},
    {'\'', {{4,4,2,0,0,0,0}}},
    {'+', {{0,4,4,31,4,4,0}}}, {'?', {{14,17,1,2,4,0,4}}},
};

void DrawText(SDL_Renderer* r, int x, int y, int scale, RGB c, const std::string& text) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
    int cx = x;
    for (char raw : text) {
        char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
        auto it = kFont.find(ch);
        const Glyph& g = it == kFont.end() ? kFont.at(' ') : it->second;
        for (int ry = 0; ry < 7; ++ry) for (int rx = 0; rx < 5; ++rx) {
            if (g.rows[ry] & (1 << (4 - rx))) {
                SDL_Rect px{cx + rx * scale, y + ry * scale, scale, scale};
                SDL_RenderFillRect(r, &px);
            }
        }
        cx += 6 * scale;
    }
}

float MidiToFreq(int midi) { return 440.0f * std::pow(2.0f, (midi - 69) / 12.0f); }
std::array<int, 3> ChordForDegree(int degree) {
    static const std::array<std::array<int, 3>, 7> triads = {{{{0,4,7}},{{2,5,9}},{{4,7,11}},{{5,9,0}},{{7,11,2}},{{9,0,4}},{{11,2,5}}}};
    return triads[std::clamp(degree - 1, 0, 6)];
}
bool InScale(int semitone) {
    static const std::array<int,7> major = {0,2,4,5,7,9,11};
    semitone = (semitone % 12 + 12) % 12;
    return std::find(major.begin(), major.end(), semitone) != major.end();
}

RGB HsvToRgb(float h, float s, float v) {
    h = std::fmod(h, 1.0f);
    if (h < 0.0f) h += 1.0f;
    const float c = v * s;
    const float hp = h * 6.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (hp < 1.0f) { r = c; g = x; }
    else if (hp < 2.0f) { r = x; g = c; }
    else if (hp < 3.0f) { g = c; b = x; }
    else if (hp < 4.0f) { g = x; b = c; }
    else if (hp < 5.0f) { r = x; b = c; }
    else { r = c; b = x; }
    const float m = v - c;
    return {static_cast<uint8_t>((r + m) * 255.0f),
            static_cast<uint8_t>((g + m) * 255.0f),
            static_cast<uint8_t>((b + m) * 255.0f)};
}

struct BarMetrics {
    float timingErrSum = 0.0f;
    int timingSamples = 0;
    int harmonyHits = 0;
    int harmonyTotal = 0;
    int triggeredKick = 0;
    int triggeredSnare = 0;
    int triggeredHat = 0;
};

enum class Objective { LockGroove, Cadence, SparseBar, SyncHit };
enum class TutorialStep { MoveMacro, PlaceMacro, MoveMicro, PlaceMicro, PlayOneBar, Complete };
enum class GameMode { TitleScreen, GridCoop, TestCardCooldown, DuelArena, RailSignalRush, SignalForge, NuclearRhythmWar, SnakeDuet, LongJumpDuet };
enum class DuelAction { None, MoveLeft, MoveRight, Attack, Block };
enum class SignalPhase { Plan, Execute, Review };

struct GameState;
void DrawPanel(SDL_Renderer* r, int x, int y, int w, int h, RGB fill, RGB border);
void BeginNewBar(GameState& g);
void SpawnBigExplosion(GameState& g, float x, float y, RGB core, RGB outer);
void BuildSignalPuzzlePhrase(GameState& g);
void InitTitleScreen(GameState& g, uint32_t nowTicks);
void GenerateSignalCircuit(GameState& g);
void InitNuclearMode(GameState& g, uint32_t nowTicks);
void InitSnakeMode(GameState& g, uint32_t nowTicks);
void InitLongJumpMode(GameState& g, uint32_t nowTicks);
void ResetLongJumpRound(GameState& g);

std::string ObjectiveName(Objective o) {
    switch (o) {
        case Objective::LockGroove: return "LOCK GROOVE";
        case Objective::Cadence: return "CADENCE V-I";
        case Objective::SparseBar: return "SPARSE BAR";
        case Objective::SyncHit: return "SYNC HIT";
    }
    return "NONE";
}
std::string TutorialTitle(TutorialStep s) {
    switch (s) {
        case TutorialStep::MoveMacro: return "STEP 1: P1 MOVE";
        case TutorialStep::PlaceMacro: return "STEP 2: P1 PLACE";
        case TutorialStep::MoveMicro: return "STEP 3: P2 MOVE";
        case TutorialStep::PlaceMicro: return "STEP 4: P2 TOGGLE";
        case TutorialStep::PlayOneBar: return "STEP 5: LET IT LOOP";
        case TutorialStep::Complete: return "TUTORIAL COMPLETE";
    }
    return "TUTORIAL";
}

std::string DuelActionName(DuelAction a) {
    switch (a) {
        case DuelAction::MoveLeft: return "LEFT";
        case DuelAction::MoveRight: return "RIGHT";
        case DuelAction::Attack: return "ATTACK";
        case DuelAction::Block: return "BLOCK";
        case DuelAction::None: break;
    }
    return "-";
}

struct GameState {
    std::array<int, kMacroCols> macroChordByBar{};
    std::array<std::array<bool, kMicroCols>, kMicroRows> micro{};
    int macroCursorCol = 0;
    int macroSelectedRow = 0;
    int microCursorCol = 0;
    int microSelectedRow = 0;

    int score = 0;
    int combo = 0;
    int objectiveBarCounter = 0;
    Objective objective = Objective::LockGroove;

    int currentBar = 0;
    int currentStep = 0;
    int prevChord = 1;
    int activeChord = 1;

    float songSeconds = 0.0f;
    float bpm = kStartBpm;
    bool running = true;
    bool paused = false;
    int roundBeatsTotal = static_cast<int>(kRoundDurationSec * (kStartBpm / 60.0f));
    int roundBeatsRemaining = static_cast<int>(kRoundDurationSec * (kStartBpm / 60.0f));
    GameMode mode = GameMode::GridCoop;
    GameMode cooldownNextMode = GameMode::GridCoop;
    float intermissionCountdown = 0.0f;
    bool gridPlayed = false;
    bool duelPlayed = false;
    bool railPlayed = false;
    bool signalPlayed = false;
    bool nuclearPlayed = false;
    bool snakePlayed = false;
    bool longJumpPlayed = false;
    int titleSelection = 0;  // 0 grid, 1 duel, 2 rail, 3 signal, 4 nuclear, 5 snake, 6 longjump

    uint32_t lastFrameTicks = 0;
    uint32_t barStartTicks = 0;
    uint32_t lastMacroCommitTicks = 0;
    uint32_t lastMicroCommitTicks = 0;

    BarMetrics metrics{};

    bool tutorialEnabled = true;
    TutorialStep tutorialStep = TutorialStep::MoveMacro;
    int tutorialMacroMoves = 0;
    int tutorialMacroPlacements = 0;
    int tutorialMicroMoves = 0;
    int tutorialMicroPlacements = 0;
    int tutorialBarStart = 0;

    float uiFlash = 0.0f;
    float beatPulse = 0.0f;
    float comboPulse = 0.0f;
    float macroPulse = 0.0f;
    float beatRipple = 0.0f;

    std::vector<Particle> particles{};
    std::vector<HitEvent> pendingHits{};
    std::mt19937 visualRng{1337};

    // Duel mode state (Game 2).
    float duelBpm = 128.0f;
    uint32_t duelBeatStartTicks = 0;
    uint32_t duelStepStartTicks = 0;
    int duelBeatsTotal = 192;
    int duelBeatsRemaining = 192;
    int duelWinner = 0;  // 0 none, 1 p1, 2 p2
    float duelEndDelay = 0.0f;
    float duelOutcomeAge = 0.0f;  // seconds since winner decided

    int p1Hp = 1000;
    int p2Hp = 1000;
    int p1Guard = 320;
    int p2Guard = 320;
    int p1Meter = 0;
    int p2Meter = 0;
    int p1Pos = -1;
    int p2Pos = 1;
    DuelAction p1Queued = DuelAction::None;
    DuelAction p2Queued = DuelAction::None;
    int p1Quality = 0;  // 2 perfect, 1 good, 0 miss
    int p2Quality = 0;
    float p1Flash = 0.0f;
    float p2Flash = 0.0f;
    float p1HitBurst = 0.0f;
    float p2HitBurst = 0.0f;
    int pendingP1HitBursts = 0;
    int pendingP2HitBursts = 0;
    int pendingKoExplosionMask = 0;  // bit 0 -> P1, bit 1 -> P2
    int duelStep = 0;
    int acidRoot = 36;
    std::array<int, 16> acidPattern = {{0, 7, 10, 5, 12, 3, 8, 2, 10, 1, 7, 14, 5, 9, 4, 11}};
    std::array<int, 16> hookA = {{0, 0, 7, 10, 12, 10, 7, 5, 7, 7, 10, 12, 14, 12, 10, 7}};
    std::array<int, 16> hookB = {{0, 0, 7, 10, 12, 10, 7, 3, 5, 7, 8, 10, 12, 10, 8, 5}};
    float duelBuild = 0.0f;  // 0..1, rises on successful attacks and increases music intensity

    // Rail mode state (Game 3).
    float railBpm = 112.0f;
    uint32_t railBeatStartTicks = 0;
    uint32_t railStepStartTicks = 0;
    int railBeatsTotal = 168;
    int railBeatsRemaining = 168;
    float railEndDelay = 0.0f;
    int railThroughput = 0;
    int railCollisions = 0;
    int railMacroCursor = 0;  // junction 0..3
    int railMicroCursor = 0;  // junction 0..3
    std::array<int, 4> railMacroDelta = {{0, 0, 0, 0}};  // -1 up, 0 straight, +1 down
    std::array<bool, 4> railMicroFlip = {{false, false, false, false}};
    std::vector<RailTrain> railTrains{};
    int railSpawnFlip = 0;
    float railFlash = 0.0f;

    // Signal Forge mode state (Game 4).
    float signalBpm = 118.0f;
    uint32_t signalBeatStartTicks = 0;
    uint32_t signalStepStartTicks = 0;
    int signalBeatsTotal = 176;
    int signalBeatsRemaining = 176;
    float signalEndDelay = 0.0f;
    int signalClean = 0;
    int signalNoise = 0;
    int signalCombo = 0;
    float signalInterference = 0.0f;
    int signalMacroCursor = 0;  // stage 0..7
    int signalMicroCursor = 2;  // lane 0..4
    std::array<int, 8> signalRoute = {{0, 0, 0, 0, 0, 0, 0, 0}};  // -1 low, 0 mid, +1 high
    std::array<int, 8> signalTargetRoute = {{0, 0, 0, 0, 0, 0, 0, 0}};  // puzzle spec
    std::array<int, 8> signalDisturb = {{0, 0, 0, 0, 0, 0, 0, 0}};      // -1/0/+1 interference map
    int signalWindowLane = 2;
    int signalJamOffset = 0;
    int signalPhraseIndex = 0;
    int signalPlanEditsLeft = 6;
    int signalMicroHitsPhrase = 0;
    int signalMicroNeeded = 6;
    int signalPhrasesSolved = 0;
    int signalPhrasesFailed = 0;
    SignalPhase signalPhase = SignalPhase::Plan;
    int signalExecBeat = 0;           // 0..8 during execution
    int signalRunLane = 2;            // simulated lane state during execution
    int signalAcks = 0;               // successful micro locks in current run
    bool signalAckedThisBeat = false; // one commit per beat
    std::array<int, 8> signalTraceLane = {{2, 2, 2, 2, 2, 2, 2, 2}};
    std::array<bool, 8> signalTraceAck = {{false, false, false, false, false, false, false, false}};
    bool signalWindowHitThisBeat = false;

    std::array<std::array<int, kSignalCols>, kSignalRows> signalTileType{};
    std::array<std::array<int, kSignalCols>, kSignalRows> signalTileRot{};
    std::array<std::array<bool, kSignalCols>, kSignalRows> signalPowered{};
    int signalP1Col = 0;
    int signalP1Row = 2;
    int signalP2Col = kSignalCols - 1;
    int signalP2Row = 2;
    int signalPower = 0;
    int signalGlitchCooldown = 4;
    bool signalConnected = false;
    int signalTuneCursor = 8;      // 0..15
    float signalTargetFreq = 8.0f; // 0..15
    int signalTuneLock = 0;        // 0..100
    int signalTuneStreak = 0;

    // Nuclear Rhythm War mode state (Game 5).
    float nuclearBpm = 93.6f;
    uint32_t nuclearBeatStartTicks = 0;
    uint32_t nuclearStepStartTicks = 0;
    int nuclearBeatsTotal = 184;
    int nuclearBeatsRemaining = 184;
    float nuclearEndDelay = 0.0f;
    int nuclearEscalation = 0;   // 0..100
    int nuclearDevastation = 0;  // 0..100
    int nuclearStability = 100;  // 0..100
    int nuclearMacroCursor = 0;  // doctrine slot 0..7
    std::array<int, 8> nuclearDoctrine = {{0, 0, 0, 0, 0, 0, 0, 0}};  // 0 HOLD,1 PROBE,2 VOLLEY,3 ALL-IN
    int nuclearMicroCursor = 3;  // defense lane 0..7
    std::array<int, 8> nuclearThreat = {{0, 0, 0, 0, 0, 0, 0, 0}};    // threat intensity
    std::array<bool, 8> nuclearIntercept = {{false, false, false, false, false, false, false, false}};
    int nuclearCombo = 0;
    int nuclearSaved = 0;
    int nuclearMissed = 0;
    float nuclearFlash = 0.0f;
    float nuclearPsy = 0.0f;

    // Snake Duet mode state (Game 6).
    float snakeBpm = 110.0f;
    uint32_t snakeBeatStartTicks = 0;
    uint32_t snakeStepStartTicks = 0;
    int snakeBeatsTotal = 176;
    int snakeBeatsRemaining = 176;
    float snakeEndDelay = 0.0f;
    int snakeScore = 0;
    int snakeP1Score = 0;
    int snakeP2Score = 0;
    int snakeCombo = 0;
    int snakeGridW = 24;
    int snakeGridH = 16;
    std::vector<SDL_Point> snake1{};
    std::vector<SDL_Point> snake2{};
    int snakeDir1 = 1;       // 0 up,1 right,2 down,3 left
    int snakeDir2 = 3;       // 0 up,1 right,2 down,3 left
    int snakePendingDir1 = 1;
    int snakePendingDir2 = 3;
    SDL_Point snakeFood{12, 8};
    bool snakeGameOver = false;

    // Long Jump Duet mode state (Game 7).
    float longJumpBpm = 116.0f;
    uint32_t longJumpBeatStartTicks = 0;
    uint32_t longJumpStepStartTicks = 0;
    int longJumpBeatsTotal = 168;
    int longJumpBeatsRemaining = 168;
    int longJumpRound = 1;
    int longJumpP1Rounds = 0;
    int longJumpP2Rounds = 0;
    float longJumpResolve = 0.0f;
    float longJumpEndDelay = 0.0f;
    int longJumpRoundWinner = 0; // 0 draw,1 p1,2 p2

    float ljRun1 = 0.0f;   // meters to line
    float ljRun2 = 0.0f;
    float ljSpeed1 = 4.0f; // m/s
    float ljSpeed2 = 4.0f;
    float ljAngle1 = 34.0f;
    float ljAngle2 = 34.0f;
    int ljExpectedFoot1 = 0; // 0 left, 1 right
    int ljExpectedFoot2 = 1;
    int ljLastFoot1 = 0;
    int ljLastFoot2 = 1;
    int ljCombo1 = 0;
    int ljCombo2 = 0;
    std::array<float, 2> ljFootGlow1 = {{0.0f, 0.0f}};
    std::array<float, 2> ljFootGlow2 = {{0.0f, 0.0f}};
    bool ljJumped1 = false;
    bool ljJumped2 = false;
    bool ljInAir1 = false;
    bool ljInAir2 = false;
    bool ljDone1 = false;
    bool ljDone2 = false;
    bool ljFoul1 = false;
    bool ljFoul2 = false;
    float ljFlightX1 = 0.0f; // meters past line
    float ljFlightX2 = 0.0f;
    float ljFlightY1 = 0.0f;
    float ljFlightY2 = 0.0f;
    float ljVx1 = 0.0f;
    float ljVx2 = 0.0f;
    float ljVy1 = 0.0f;
    float ljVy2 = 0.0f;
    float ljDist1 = 0.0f;
    float ljDist2 = 0.0f;
};

float StepDurationSeconds(float bpm) { return 60.0f / bpm / 4.0f; }
float BarDurationSeconds(float bpm) { return StepDurationSeconds(bpm) * 16.0f; }

int ActiveMicroCells(const GameState& g) {
    int n = 0;
    for (int r = 0; r < kMicroRows; ++r) for (int c = 0; c < kMicroCols; ++c) n += g.micro[r][c] ? 1 : 0;
    return n;
}

void InitDuelMode(GameState& g, uint32_t nowTicks) {
    g.mode = GameMode::DuelArena;
    g.paused = false;
    g.duelBpm = 128.0f;
    g.duelBeatsTotal = static_cast<int>(90.0f * (g.duelBpm / 60.0f));
    g.duelBeatsRemaining = g.duelBeatsTotal;
    g.duelBeatStartTicks = nowTicks;
    g.duelStepStartTicks = nowTicks;
    g.duelWinner = 0;
    g.duelEndDelay = 0.0f;
    g.duelOutcomeAge = 0.0f;

    g.p1Hp = 1000;
    g.p2Hp = 1000;
    g.p1Guard = 320;
    g.p2Guard = 320;
    g.p1Meter = 0;
    g.p2Meter = 0;
    g.p1Pos = -1;
    g.p2Pos = 1;
    g.p1Queued = DuelAction::None;
    g.p2Queued = DuelAction::None;
    g.p1Quality = 0;
    g.p2Quality = 0;
    g.p1Flash = 0.0f;
    g.p2Flash = 0.0f;
    g.p1HitBurst = 0.0f;
    g.p2HitBurst = 0.0f;
    g.pendingP1HitBursts = 0;
    g.pendingP2HitBursts = 0;
    g.pendingKoExplosionMask = 0;
    g.duelStep = 0;
    g.duelBuild = 0.0f;
    g.duelPlayed = true;
}

void InitGridCoopMode(GameState& g, uint32_t nowTicks) {
    g.mode = GameMode::GridCoop;
    g.paused = false;
    g.score = 0;
    g.combo = 0;
    g.objectiveBarCounter = 0;
    g.objective = Objective::LockGroove;
    g.currentBar = 0;
    g.currentStep = 0;
    g.prevChord = 1;
    g.activeChord = 1;
    g.songSeconds = 0.0f;
    g.bpm = kStartBpm;
    g.roundBeatsTotal = static_cast<int>(kRoundDurationSec * (kStartBpm / 60.0f));
    g.roundBeatsRemaining = g.roundBeatsTotal;
    g.lastFrameTicks = nowTicks;
    g.barStartTicks = nowTicks;
    g.lastMacroCommitTicks = nowTicks;
    g.lastMicroCommitTicks = nowTicks;
    g.metrics = {};
    g.uiFlash = 0.0f;
    g.beatPulse = 0.0f;
    g.comboPulse = 0.0f;
    g.macroPulse = 0.0f;
    g.beatRipple = 0.0f;
    g.particles.clear();
    g.pendingHits.clear();
    g.tutorialEnabled = true;
    g.tutorialStep = TutorialStep::MoveMacro;
    g.tutorialMacroMoves = 0;
    g.tutorialMacroPlacements = 0;
    g.tutorialMicroMoves = 0;
    g.tutorialMicroPlacements = 0;
    g.tutorialBarStart = 0;
    g.macroCursorCol = 0;
    g.macroSelectedRow = 0;
    g.microCursorCol = 0;
    g.microSelectedRow = 0;
    for (int i = 0; i < kMacroCols; ++i) g.macroChordByBar[i] = (i % 2 == 0) ? 1 : 5;
    for (int r = 0; r < kMicroRows; ++r) for (int c = 0; c < kMicroCols; ++c) g.micro[r][c] = false;
    BeginNewBar(g);
    g.gridPlayed = true;
}

void InitRailMode(GameState& g, uint32_t nowTicks) {
    g.mode = GameMode::RailSignalRush;
    g.paused = false;
    g.songSeconds = 0.0f;
    g.currentBar = 0;
    g.currentStep = 0;
    g.railBpm = 112.0f;
    g.railBeatsTotal = static_cast<int>(90.0f * (g.railBpm / 60.0f));
    g.railBeatsRemaining = g.railBeatsTotal;
    g.railBeatStartTicks = nowTicks;
    g.railStepStartTicks = nowTicks;
    g.railEndDelay = 0.0f;
    g.railThroughput = 0;
    g.railCollisions = 0;
    g.railMacroCursor = 0;
    g.railMicroCursor = 0;
    g.railMacroDelta = {{0, 0, 0, 0}};
    g.railMicroFlip = {{false, false, false, false}};
    g.railTrains.clear();
    g.railSpawnFlip = 0;
    g.railFlash = 0.0f;
    g.beatPulse = 0.0f;
    g.beatRipple = 0.0f;
    g.uiFlash = 0.0f;
    g.lastFrameTicks = nowTicks;
    g.particles.clear();
    g.pendingHits.clear();
    g.railPlayed = true;
}

void InitSignalMode(GameState& g, uint32_t nowTicks) {
    g.mode = GameMode::SignalForge;
    g.paused = false;
    g.songSeconds = 0.0f;
    g.currentBar = 0;
    g.currentStep = 0;
    g.signalBpm = 118.0f;
    g.signalBeatsTotal = static_cast<int>(90.0f * (g.signalBpm / 60.0f));
    g.signalBeatsRemaining = g.signalBeatsTotal;
    g.signalBeatStartTicks = nowTicks;
    g.signalStepStartTicks = nowTicks;
    g.signalEndDelay = 0.0f;
    g.signalClean = 0;
    g.signalNoise = 0;
    g.signalCombo = 0;
    g.signalInterference = 0.0f;
    g.signalMacroCursor = 0;
    g.signalMicroCursor = 2;
    g.signalRoute = {{0, 0, 0, 0, 0, 0, 0, 0}};
    g.signalTargetRoute = {{0, 0, 1, 0, -1, 0, 1, 0}};
    g.signalDisturb = {{0, 0, 0, 0, 0, 0, 0, 0}};
    g.signalWindowLane = 2;
    g.signalJamOffset = 0;
    g.signalPhraseIndex = 0;
    g.signalPlanEditsLeft = 6;
    g.signalMicroHitsPhrase = 0;
    g.signalMicroNeeded = 6;
    g.signalPhrasesSolved = 0;
    g.signalPhrasesFailed = 0;
    g.signalPhase = SignalPhase::Execute;
    g.signalExecBeat = 0;
    g.signalRunLane = 2;
    g.signalAcks = 0;
    g.signalAckedThisBeat = false;
    g.signalTraceLane = {{2, 2, 2, 2, 2, 2, 2, 2}};
    g.signalTraceAck = {{false, false, false, false, false, false, false, false}};
    g.signalWindowHitThisBeat = false;
    g.signalPower = 20;
    g.signalGlitchCooldown = 4;
    g.signalConnected = false;
    g.signalTuneCursor = 8;
    g.signalTargetFreq = 8.0f;
    g.signalTuneLock = 0;
    g.signalTuneStreak = 0;
    GenerateSignalCircuit(g);
    g.beatPulse = 0.0f;
    g.beatRipple = 0.0f;
    g.uiFlash = 0.0f;
    g.lastFrameTicks = nowTicks;
    g.particles.clear();
    g.pendingHits.clear();
    g.signalPlayed = true;
}

void InitNuclearMode(GameState& g, uint32_t nowTicks) {
    g.mode = GameMode::NuclearRhythmWar;
    g.paused = false;
    g.songSeconds = 0.0f;
    g.currentBar = 0;
    g.currentStep = 0;
    g.nuclearBpm = 93.6f;
    g.nuclearBeatsTotal = static_cast<int>(95.0f * (g.nuclearBpm / 60.0f));
    g.nuclearBeatsRemaining = g.nuclearBeatsTotal;
    g.nuclearBeatStartTicks = nowTicks;
    g.nuclearStepStartTicks = nowTicks;
    g.nuclearEndDelay = 0.0f;
    g.nuclearEscalation = 10;
    g.nuclearDevastation = 0;
    g.nuclearStability = 100;
    g.nuclearMacroCursor = 0;
    g.nuclearDoctrine = {{0, 0, 1, 1, 0, 1, 0, 0}};
    g.nuclearMicroCursor = 3;
    g.nuclearThreat = {{0, 0, 0, 0, 0, 0, 0, 0}};
    g.nuclearIntercept = {{false, false, false, false, false, false, false, false}};
    g.nuclearCombo = 0;
    g.nuclearSaved = 0;
    g.nuclearMissed = 0;
    g.nuclearFlash = 0.0f;
    g.nuclearPsy = 0.0f;
    g.beatPulse = 0.0f;
    g.beatRipple = 0.0f;
    g.uiFlash = 0.0f;
    g.lastFrameTicks = nowTicks;
    g.particles.clear();
    g.pendingHits.clear();
    g.nuclearPlayed = true;
}

void SpawnSnakeFood(GameState& g) {
    std::vector<SDL_Point> freeCells;
    freeCells.reserve(static_cast<size_t>(g.snakeGridW * g.snakeGridH));
    for (int y = 0; y < g.snakeGridH; ++y) {
        for (int x = 0; x < g.snakeGridW; ++x) {
            bool blocked = false;
            for (const auto& c : g.snake1) if (c.x == x && c.y == y) { blocked = true; break; }
            if (!blocked) for (const auto& c : g.snake2) if (c.x == x && c.y == y) { blocked = true; break; }
            if (!blocked) freeCells.push_back({x, y});
        }
    }
    if (freeCells.empty()) return;
    std::uniform_int_distribution<int> pick(0, static_cast<int>(freeCells.size()) - 1);
    g.snakeFood = freeCells[pick(g.visualRng)];
}

void InitSnakeMode(GameState& g, uint32_t nowTicks) {
    g.mode = GameMode::SnakeDuet;
    g.paused = false;
    g.songSeconds = 0.0f;
    g.currentBar = 0;
    g.currentStep = 0;
    g.snakeBpm = 110.0f;
    g.snakeBeatsTotal = static_cast<int>(95.0f * (g.snakeBpm / 60.0f));
    g.snakeBeatsRemaining = g.snakeBeatsTotal;
    g.snakeBeatStartTicks = nowTicks;
    g.snakeStepStartTicks = nowTicks;
    g.snakeEndDelay = 0.0f;
    g.snakeScore = 0;
    g.snakeP1Score = 0;
    g.snakeP2Score = 0;
    g.snakeCombo = 0;
    g.snakeGridW = 24;
    g.snakeGridH = 16;
    g.snake1 = {{4, 8}, {3, 8}, {2, 8}};
    g.snake2 = {{19, 8}, {20, 8}, {21, 8}};
    g.snakeDir1 = 1;
    g.snakeDir2 = 3;
    g.snakePendingDir1 = 1;
    g.snakePendingDir2 = 3;
    g.snakeGameOver = false;
    g.beatPulse = 0.0f;
    g.beatRipple = 0.0f;
    g.uiFlash = 0.0f;
    g.lastFrameTicks = nowTicks;
    g.particles.clear();
    g.pendingHits.clear();
    SpawnSnakeFood(g);
    g.snakePlayed = true;
}

void ResetLongJumpRound(GameState& g) {
    g.ljRun1 = 0.0f;
    g.ljRun2 = 0.0f;
    g.ljSpeed1 = 4.0f;
    g.ljSpeed2 = 4.0f;
    g.ljAngle1 = 34.0f;
    g.ljAngle2 = 34.0f;
    g.ljExpectedFoot1 = 0;
    g.ljExpectedFoot2 = 1;
    g.ljLastFoot1 = 0;
    g.ljLastFoot2 = 1;
    g.ljCombo1 = 0;
    g.ljCombo2 = 0;
    g.ljFootGlow1 = {{0.0f, 0.0f}};
    g.ljFootGlow2 = {{0.0f, 0.0f}};
    g.ljJumped1 = false;
    g.ljJumped2 = false;
    g.ljInAir1 = false;
    g.ljInAir2 = false;
    g.ljDone1 = false;
    g.ljDone2 = false;
    g.ljFoul1 = false;
    g.ljFoul2 = false;
    g.ljFlightX1 = 0.0f;
    g.ljFlightX2 = 0.0f;
    g.ljFlightY1 = 0.0f;
    g.ljFlightY2 = 0.0f;
    g.ljVx1 = 0.0f;
    g.ljVx2 = 0.0f;
    g.ljVy1 = 0.0f;
    g.ljVy2 = 0.0f;
    g.ljDist1 = 0.0f;
    g.ljDist2 = 0.0f;
}

void InitLongJumpMode(GameState& g, uint32_t nowTicks) {
    g.mode = GameMode::LongJumpDuet;
    g.paused = false;
    g.songSeconds = 0.0f;
    g.currentBar = 0;
    g.currentStep = 0;
    g.longJumpBpm = 116.0f;
    g.longJumpBeatsTotal = static_cast<int>(90.0f * (g.longJumpBpm / 60.0f));
    g.longJumpBeatsRemaining = g.longJumpBeatsTotal;
    g.longJumpBeatStartTicks = nowTicks;
    g.longJumpStepStartTicks = nowTicks;
    g.longJumpRound = 1;
    g.longJumpP1Rounds = 0;
    g.longJumpP2Rounds = 0;
    g.longJumpResolve = 0.0f;
    g.longJumpEndDelay = 0.0f;
    g.longJumpRoundWinner = 0;
    ResetLongJumpRound(g);
    g.beatPulse = 0.0f;
    g.beatRipple = 0.0f;
    g.uiFlash = 0.0f;
    g.lastFrameTicks = nowTicks;
    g.particles.clear();
    g.pendingHits.clear();
    g.longJumpPlayed = true;
}

void InitTitleScreen(GameState& g, uint32_t nowTicks) {
    g.mode = GameMode::TitleScreen;
    g.paused = false;
    g.songSeconds = 0.0f;
    g.lastFrameTicks = nowTicks;
    g.gridPlayed = false;
    g.duelPlayed = false;
    g.railPlayed = false;
    g.signalPlayed = false;
    g.nuclearPlayed = false;
    g.snakePlayed = false;
    g.longJumpPlayed = false;
    g.titleSelection = 0;
    g.particles.clear();
}

bool AllGameModesPlayed(const GameState& g) {
    return g.gridPlayed && g.duelPlayed && g.railPlayed && g.signalPlayed && g.nuclearPlayed && g.snakePlayed && g.longJumpPlayed;
}

GameMode NextUnplayedMode(const GameState& g) {
    if (!g.gridPlayed) return GameMode::GridCoop;
    if (!g.duelPlayed) return GameMode::DuelArena;
    if (!g.railPlayed) return GameMode::RailSignalRush;
    if (!g.signalPlayed) return GameMode::SignalForge;
    if (!g.nuclearPlayed) return GameMode::NuclearRhythmWar;
    if (!g.snakePlayed) return GameMode::SnakeDuet;
    if (!g.longJumpPlayed) return GameMode::LongJumpDuet;
    return GameMode::GridCoop;
}

GameMode RandomUnplayedMode(GameState& g) {
    std::vector<GameMode> pool;
    if (!g.gridPlayed) pool.push_back(GameMode::GridCoop);
    if (!g.duelPlayed) pool.push_back(GameMode::DuelArena);
    if (!g.railPlayed) pool.push_back(GameMode::RailSignalRush);
    if (!g.signalPlayed) pool.push_back(GameMode::SignalForge);
    if (!g.nuclearPlayed) pool.push_back(GameMode::NuclearRhythmWar);
    if (!g.snakePlayed) pool.push_back(GameMode::SnakeDuet);
    if (!g.longJumpPlayed) pool.push_back(GameMode::LongJumpDuet);
    if (pool.empty()) return GameMode::GridCoop;
    std::uniform_int_distribution<int> dist(0, static_cast<int>(pool.size()) - 1);
    return pool[dist(g.visualRng)];
}

void InitTestCardCooldown(GameState& g, GameMode nextMode) {
    g.mode = GameMode::TestCardCooldown;
    g.paused = false;
    g.intermissionCountdown = 5.0f;
    g.cooldownNextMode = nextMode;
}

int BeatQuality(float secIntoBeat, float beatDurSec) {
    const float d = std::min(secIntoBeat, beatDurSec - secIntoBeat);
    if (d <= 0.035f) return 2;
    if (d <= 0.090f) return 1;
    return 0;
}

int AttackDamageForQuality(int q) {
    if (q >= 2) return 72;
    if (q == 1) return 44;
    return 20;
}

void TriggerDuelGrooveStep(GameState& g, JuceAudioEngine& audio) {
    const int s = g.duelStep % 16;
    const float build = std::clamp(g.duelBuild, 0.0f, 1.0f);
    const bool useHookB = ((g.currentBar / 2) % 2) == 1;

    // Big-break kick pattern with syncopated extra hits.
    if (s == 0 || s == 3 || s == 8 || s == 10 || s == 12 || (s == 15 && (g.currentBar % 2 == 1))) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 148.0f, 0.90f + 0.30f * build, 9.2f, 0.28f);
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, 1900.0f, 0.07f + 0.05f * build, 130.0f, 0.015f);
    }
    if (build > 0.45f && (s == 6 || s == 14)) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 124.0f, 0.48f + 0.20f * build, 14.0f, 0.13f);
    }

    // Snare backbeat + occasional flam/ghost for erratic feel.
    if (s == 4 || s == 12 || (s == 11 && (g.currentBar % 3 == 2))) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.46f + 0.18f * build, 33.0f, 0.11f);
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, 2300.0f, 0.09f + 0.05f * build, 140.0f, 0.012f);
        if (s == 12 && (g.currentBar % 4 == 3)) {
            TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.20f + 0.09f * build, 65.0f, 0.035f);
        }
    }

    // Fast hats with slight irregular accents.
    const bool baseHat = (s % 2 == 0) || (s == 5) || (s == 13);
    const bool extraHat = (build > 0.35f && (s % 2 == 1)) || (build > 0.70f && (s == 2 || s == 6 || s == 10 || s == 14));
    if (baseHat || extraHat) {
        const float hatGain = ((s % 4 == 0) ? 0.16f : 0.10f) + 0.10f * build;
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, hatGain, 95.0f, 0.026f);
    }

    // Ear-worm hook lead: repeated motif with bar-level call/response.
    const int hookSemitone = (useHookB ? g.hookB[s] : g.hookA[s]) % 24;
    const int note = g.acidRoot + hookSemitone;
    const float accent = (s == 0 || s == 6 || s == 10 || s == 14) ? 1.0f : 0.75f;
    TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(note + 12), (0.18f + 0.14f * build) * accent, 15.0f - 4.0f * build, 0.095f);
    // Subline follows with a slightly different contour for glue.
    const int subSemitone = g.acidPattern[s] % 24;
    TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(g.acidRoot + subSemitone), 0.10f + 0.08f * build, 18.0f, 0.08f);
    if (build > 0.40f) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(note + 19), 0.06f + 0.08f * build, 20.0f, 0.05f);
    }
    if (s == 7 || s == 15) {
        // Cadence stabs make phrase endings memorable.
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(g.acidRoot + 12), 0.10f + 0.08f * build, 24.0f, 0.045f);
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(g.acidRoot + 19), 0.07f + 0.05f * build, 26.0f, 0.040f);
    }
    if (build > 0.78f && s == 15) {
        // High-energy turnaround burst before the next bar.
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.32f, 55.0f, 0.05f);
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(note + 19), 0.16f, 30.0f, 0.035f);
    }
}

void ResolveDuelBeat(GameState& g, JuceAudioEngine& audio) {
    const bool p1Block = (g.p1Queued == DuelAction::Block && g.p1Quality > 0);
    const bool p2Block = (g.p2Queued == DuelAction::Block && g.p2Quality > 0);

    auto doMove = [](int& pos, DuelAction action, int quality) {
        if (quality <= 0) return;
        if (action == DuelAction::MoveLeft) pos--;
        if (action == DuelAction::MoveRight) pos++;
        pos = std::clamp(pos, -3, 3);
    };
    doMove(g.p1Pos, g.p1Queued, g.p1Quality);
    doMove(g.p2Pos, g.p2Queued, g.p2Quality);

    const bool inRange = std::abs(g.p1Pos - g.p2Pos) <= 2;
    const bool p1Atk = (g.p1Queued == DuelAction::Attack && g.p1Quality > 0);
    const bool p2Atk = (g.p2Queued == DuelAction::Attack && g.p2Quality > 0);

    bool p1Success = false;
    bool p2Success = false;

    if (p1Atk && inRange) {
        const int dmg = AttackDamageForQuality(g.p1Quality);
        if (p2Block) {
            g.p2Guard -= (g.p1Quality == 2 ? 86 : 58);
            g.p2Hp -= (g.p1Quality == 2 ? 8 : 5);  // chip damage so blocked hits still cost health
            if (g.p2Guard < 0) {
                g.p2Hp += g.p2Guard / 2;
                g.p2Guard = 0;
            }
            TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.22f, 45.0f, 0.05f);
            p1Success = true;
            g.p2Flash = std::max(g.p2Flash, 0.7f);
            g.p2HitBurst = std::max(g.p2HitBurst, 0.8f);
            g.pendingP2HitBursts++;
        } else {
            g.p2Hp -= dmg;
            g.p1Meter = std::min(100, g.p1Meter + (g.p1Quality == 2 ? 12 : 7));
            TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 140.0f, 0.55f, 14.0f, 0.12f);
            TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, 880.0f, 0.12f, 35.0f, 0.04f);
            g.p2Flash = 1.0f;
            g.p2HitBurst = 1.0f;
            g.pendingP2HitBursts++;
            p1Success = true;
        }
    }
    if (p2Atk && inRange) {
        const int dmg = AttackDamageForQuality(g.p2Quality);
        if (p1Block) {
            g.p1Guard -= (g.p2Quality == 2 ? 86 : 58);
            g.p1Hp -= (g.p2Quality == 2 ? 8 : 5);  // chip damage so blocked hits still cost health
            if (g.p1Guard < 0) {
                g.p1Hp += g.p1Guard / 2;
                g.p1Guard = 0;
            }
            TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.22f, 45.0f, 0.05f);
            p2Success = true;
            g.p1Flash = std::max(g.p1Flash, 0.7f);
            g.p1HitBurst = std::max(g.p1HitBurst, 0.8f);
            g.pendingP1HitBursts++;
        } else {
            g.p1Hp -= dmg;
            g.p2Meter = std::min(100, g.p2Meter + (g.p2Quality == 2 ? 12 : 7));
            TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 140.0f, 0.55f, 14.0f, 0.12f);
            TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, 880.0f, 0.12f, 35.0f, 0.04f);
            g.p1Flash = 1.0f;
            g.p1HitBurst = 1.0f;
            g.pendingP1HitBursts++;
            p2Success = true;
        }
    }

    if (p1Success) g.duelBuild = std::min(1.0f, g.duelBuild + (g.p1Quality == 2 ? 0.10f : 0.07f));
    if (p2Success) g.duelBuild = std::min(1.0f, g.duelBuild + (g.p2Quality == 2 ? 0.10f : 0.07f));
    if (p1Success && p2Success) g.duelBuild = std::min(1.0f, g.duelBuild + 0.06f);

    if (p1Block) g.p1Meter = std::min(100, g.p1Meter + 3);
    if (p2Block) g.p2Meter = std::min(100, g.p2Meter + 3);

    g.p1Hp = std::max(0, g.p1Hp);
    g.p2Hp = std::max(0, g.p2Hp);

    if (g.p1Hp <= 0) g.pendingKoExplosionMask |= 1;
    if (g.p2Hp <= 0) g.pendingKoExplosionMask |= 2;

    g.p1Queued = DuelAction::None;
    g.p2Queued = DuelAction::None;
    g.p1Quality = 0;
    g.p2Quality = 0;
}

constexpr int kRailGridMinCell = 0;
constexpr int kRailGridMaxCell = 3;
constexpr std::array<int, 4> kRailJunctionCells = {{0, 1, 2, 3}};
constexpr int kRailCellStepPx = 300;
constexpr int kRailSpawnCellLeft = kRailGridMinCell - 1;
constexpr int kRailSpawnCellRight = kRailGridMaxCell + 1;
constexpr int kRailTrackSpanPx = (kRailGridMaxCell - kRailGridMinCell) * kRailCellStepPx;
constexpr int kRailTrackLeftX = kRailArenaX + (kRailArenaW - kRailTrackSpanPx) / 2;
constexpr int kRailTrackRightX = kRailTrackLeftX + kRailTrackSpanPx;

float RailCellX(float cell) {
    return static_cast<float>(kRailTrackLeftX) +
           (cell - static_cast<float>(kRailGridMinCell)) * static_cast<float>(kRailCellStepPx);
}

int RailJunctionX(int index) {
    const int i = std::clamp(index, 0, 3);
    return static_cast<int>(std::round(RailCellX(static_cast<float>(kRailJunctionCells[i]))));
}

int RailLaneY(int arenaY, int arenaH, int lane) {
    const int top = arenaY + 44;
    const int gap = (arenaH - 88) / 4;
    return top + std::clamp(lane, 0, 4) * gap;
}

int RailLaneCenterY(int lane) {
    // Centerline for each lane. All rail visuals and collisions should anchor here.
    return RailLaneY(kRailArenaY, kRailArenaH, lane) + 3;
}

int RailSnapCell(float x) {
    const float f = (x - static_cast<float>(kRailTrackLeftX)) / static_cast<float>(kRailCellStepPx);
    return std::clamp(static_cast<int>(std::lround(f)), kRailGridMinCell, kRailGridMaxCell);
}

int RailSnapLane(float y) {
    int bestLane = 0;
    int bestDist = std::abs(static_cast<int>(std::lround(y)) - RailLaneCenterY(0));
    for (int lane = 1; lane < 5; ++lane) {
        const int d = std::abs(static_cast<int>(std::lround(y)) - RailLaneCenterY(lane));
        if (d < bestDist) {
            bestDist = d;
            bestLane = lane;
        }
    }
    return bestLane;
}

void SpawnRailTrain(GameState& g, int dir) {
    std::uniform_int_distribution<int> laneDist(0, 4);
    const int lane = laneDist(g.visualRng);
    RailTrain t;
    t.dir = dir;
    t.lane = lane;
    t.speed = 160.0f + g.railBpm * 0.85f;
    t.cell = (dir > 0) ? kRailSpawnCellLeft : kRailSpawnCellRight;
    t.prevCell = t.cell;
    t.nextJunction = (dir > 0) ? 0 : 3;
    t.color = (dir > 0) ? RGB{255, 214, 116} : RGB{120, 232, 255};
    g.railTrains.push_back(t);
}

void AdvanceRailBeat(GameState& g, JuceAudioEngine& audio) {
    std::vector<bool> remove(g.railTrains.size(), false);
    const size_t n = g.railTrains.size();

    auto markCollision = [&](size_t i, size_t j, float hitCell, int lane) {
        if (remove[i] || remove[j]) return;
        remove[i] = true;
        remove[j] = true;
        g.railCollisions++;
        g.railFlash = 1.0f;
        SpawnBigExplosion(g,
                          RailCellX(hitCell),
                          static_cast<float>(RailLaneCenterY(lane)),
                          {255, 120, 120},
                          {255, 240, 170});
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.28f, 25.0f, 0.10f);
    };

    for (size_t i = 0; i < n; ++i) {
        if (remove[i]) continue;
        auto& t = g.railTrains[i];
        t.prevCell = t.cell;
        t.cell += t.dir;
        while (t.nextJunction >= 0 && t.nextJunction < static_cast<int>(kRailJunctionCells.size())) {
            const int j = t.nextJunction;
            const int jc = kRailJunctionCells[j];
            const bool passed = (t.dir > 0) ? (t.cell >= jc) : (t.cell <= jc);
            if (!passed) break;

            int delta = g.railMacroDelta[j];
            if (g.railMicroFlip[j]) delta = -delta;
            t.lane = std::clamp(t.lane + delta, 0, 4);
            t.nextJunction += (t.dir > 0) ? 1 : -1;
        }
    }

    for (size_t i = 0; i < g.railTrains.size(); ++i) {
        for (size_t j = i + 1; j < g.railTrains.size(); ++j) {
            if (g.railTrains[i].lane != g.railTrains[j].lane) continue;
            if (g.railTrains[i].dir == g.railTrains[j].dir) continue;
            if (remove[i] || remove[j]) continue;
            if (g.railTrains[i].cell != g.railTrains[j].cell) continue;
            markCollision(i, j, static_cast<float>(g.railTrains[i].cell), g.railTrains[i].lane);
        }
    }

    for (size_t i = 0; i < g.railTrains.size(); ++i) {
        if (remove[i]) continue;
        const auto& t = g.railTrains[i];
        const bool exited = (t.cell < kRailSpawnCellLeft) || (t.cell > kRailSpawnCellRight);
        if (exited) {
            remove[i] = true;
            g.railThroughput++;
            g.uiFlash = std::min(1.0f, g.uiFlash + 0.05f);
        }
    }

    std::vector<RailTrain> kept;
    kept.reserve(g.railTrains.size());
    for (size_t i = 0; i < g.railTrains.size(); ++i) {
        if (!remove[i]) kept.push_back(g.railTrains[i]);
    }
    g.railTrains.swap(kept);
}

void TriggerRailGrooveStep(GameState& g, JuceAudioEngine& audio) {
    const int s = g.currentStep % 16;
    const float energy = std::clamp((g.railBpm - 112.0f) / 56.0f, 0.0f, 1.0f);

    // Straight 4/4 kick (four-on-the-floor).
    if (s == 0 || s == 4 || s == 8 || s == 12) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 84.0f, 0.30f + 0.08f * energy, 11.0f, 0.16f);
    }

    // Offbeat hi-hat.
    if (s == 2 || s == 6 || s == 10 || s == 14) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.11f + 0.04f * energy, 115.0f, 0.016f);
    }

    // Backbeat clap/snare (beats 2 and 4).
    if (s == 4 || s == 12) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.18f + 0.05f * energy, 32.0f, 0.06f);
    }

    // 16th hat ticks for forward motion.
    if ((s % 2) == 1) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.045f + 0.02f * energy, 180.0f, 0.010f);
    }

    // Minimal techno bass stab sequence.
    static const std::array<int, 16> bassPattern = {{0, 0, 3, 0, 5, 0, 3, 0, 7, 0, 3, 0, 5, 0, 3, 0}};
    if (s % 2 == 0) {
        const int midi = 40 + bassPattern[s];
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(midi), 0.13f + 0.05f * energy, 16.0f, 0.10f);
    }

    // Sparse lead pulse.
    if (s == 8 || (energy > 0.55f && s == 15)) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(64), 0.07f + 0.03f * energy, 24.0f, 0.08f);
    }
}

void TriggerSignalGrooveStep(GameState& g, JuceAudioEngine& audio) {
    const int s = g.currentStep % 16;
    const float interference = std::clamp(g.signalInterference, 0.0f, 1.0f);
    const float confidence = std::clamp(static_cast<float>(g.signalClean - g.signalNoise) / 40.0f, -1.0f, 1.0f);
    const float lift = std::clamp(0.55f + confidence * 0.45f, 0.15f, 1.0f);

    if (s == 0 || s == 4 || s == 8 || s == 12) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 90.0f, 0.28f + 0.07f * lift, 12.0f, 0.16f);
    }
    if (s == 4 || s == 12) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.14f + 0.08f * lift, 34.0f, 0.06f);
    }
    if (s == 2 || s == 6 || s == 10 || s == 14) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.08f + 0.04f * lift, 130.0f, 0.016f);
    }
    if ((s % 2) == 1) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.03f + 0.02f * lift, 210.0f, 0.010f);
    }

    static const std::array<int, 16> bassPattern = {{0, 0, 3, 0, 7, 0, 5, 0, 0, 0, 10, 0, 7, 0, 5, 0}};
    if ((s % 2) == 0) {
        const int midi = 42 + bassPattern[s];
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(midi), 0.10f + 0.04f * lift, 18.0f, 0.12f);
    }

    if (g.signalWindowHitThisBeat && (s == 0 || s == 8)) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(69), 0.07f, 22.0f, 0.08f);
    }
    if (interference > 0.35f && (s == 3 || s == 11)) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.06f + 0.11f * interference, 11.0f, 0.018f);
    }
}

enum SignalDirMask { SD_N = 1, SD_E = 2, SD_S = 4, SD_W = 8 };

int RotateMaskCW(int m) {
    int out = 0;
    if ((m & SD_N) != 0) out |= SD_E;
    if ((m & SD_E) != 0) out |= SD_S;
    if ((m & SD_S) != 0) out |= SD_W;
    if ((m & SD_W) != 0) out |= SD_N;
    return out;
}

int SignalBaseMaskForType(int type) {
    switch (type) {
        case 1: return SD_E | SD_W;                 // straight
        case 2: return SD_N | SD_E;                 // elbow
        case 3: return SD_N | SD_E | SD_W;          // tee
        case 4: return SD_N | SD_E | SD_S | SD_W;   // cross
        default: return 0;                          // empty
    }
}

int SignalCellMask(const GameState& g, int row, int col) {
    int m = SignalBaseMaskForType(g.signalTileType[row][col]);
    const int rot = g.signalTileRot[row][col] & 3;
    for (int i = 0; i < rot; ++i) m = RotateMaskCW(m);
    return m;
}

void GenerateSignalCircuit(GameState& g) {
    std::uniform_int_distribution<int> typeDist(0, 4);
    std::uniform_int_distribution<int> rotDist(0, 3);
    std::uniform_int_distribution<int> coreTypeDist(1, 4);
    for (int r = 0; r < kSignalRows; ++r) {
        for (int c = 0; c < kSignalCols; ++c) {
            g.signalTileType[r][c] = typeDist(g.visualRng);
            g.signalTileRot[r][c] = rotDist(g.visualRng);
            g.signalPowered[r][c] = false;
        }
    }
    // Ensure there is a valid possible trunk path across center row when rotated correctly.
    for (int c = 0; c < kSignalCols; ++c) {
        g.signalTileType[2][c] = coreTypeDist(g.visualRng);
    }
    g.signalP1Col = 0;
    g.signalP1Row = 2;
    g.signalP2Col = kSignalCols - 1;
    g.signalP2Row = 2;
}

std::pair<bool, int> EvaluateSignalCircuit(GameState& g) {
    for (int r = 0; r < kSignalRows; ++r) for (int c = 0; c < kSignalCols; ++c) g.signalPowered[r][c] = false;

    auto inside = [](int rr, int cc) { return rr >= 0 && rr < kSignalRows && cc >= 0 && cc < kSignalCols; };
    const int dr[4] = {-1, 0, 1, 0};
    const int dc[4] = {0, 1, 0, -1};
    const int bit[4] = {SD_N, SD_E, SD_S, SD_W};
    const int opp[4] = {SD_S, SD_W, SD_N, SD_E};

    std::vector<std::pair<int, int>> q;
    size_t qi = 0;
    if ((SignalCellMask(g, 2, 0) & SD_W) != 0) {
        g.signalPowered[2][0] = true;
        q.push_back({2, 0});
    }

    while (qi < q.size()) {
        const int r = q[qi].first;
        const int c = q[qi].second;
        qi++;
        const int m = SignalCellMask(g, r, c);
        for (int d = 0; d < 4; ++d) {
            if ((m & bit[d]) == 0) continue;
            const int nr = r + dr[d];
            const int nc = c + dc[d];
            if (!inside(nr, nc)) continue;
            const int nm = SignalCellMask(g, nr, nc);
            if ((nm & opp[d]) == 0) continue;
            if (!g.signalPowered[nr][nc]) {
                g.signalPowered[nr][nc] = true;
                q.push_back({nr, nc});
            }
        }
    }

    int poweredCells = 0;
    for (int r = 0; r < kSignalRows; ++r) for (int c = 0; c < kSignalCols; ++c) poweredCells += g.signalPowered[r][c] ? 1 : 0;
    const bool sinkReached = g.signalPowered[2][kSignalCols - 1] && ((SignalCellMask(g, 2, kSignalCols - 1) & SD_E) != 0);
    return {sinkReached, poweredCells};
}

char SignalBandChar(int v) {
    if (v > 0) return 'H';
    if (v < 0) return 'L';
    return 'M';
}

void BuildSignalPuzzlePhrase(GameState& g) {
    static const std::array<std::array<int, 8>, 6> kTargetBank = {{
        {{0, 1, 0, -1, 0, 1, 0, -1}},
        {{1, 0, -1, 0, 1, 0, -1, 0}},
        {{0, 0, 1, 1, 0, -1, -1, 0}},
        {{1, 1, 0, -1, -1, 0, 1, 0}},
        {{-1, 0, 1, 0, -1, 0, 1, 0}},
        {{0, -1, 0, 1, 0, -1, 0, 1}},
    }};
    static const std::array<std::array<int, 8>, 5> kDisturbBank = {{
        {{0, 0, 0, 0, 0, 0, 0, 0}},
        {{0, 0, 1, 0, 0, -1, 0, 0}},
        {{0, -1, 0, 1, 0, -1, 0, 1}},
        {{1, 0, -1, 0, 1, 0, -1, 0}},
        {{0, 1, 0, -1, 0, 1, 0, -1}},
    }};

    const int targetIdx = g.signalPhraseIndex % static_cast<int>(kTargetBank.size());
    const int disturbBase = std::clamp(g.signalPhrasesSolved / 2, 0, static_cast<int>(kDisturbBank.size()) - 1);
    const int disturbIdx = std::min(static_cast<int>(kDisturbBank.size()) - 1, disturbBase + (g.signalPhraseIndex % 2));

    g.signalTargetRoute = kTargetBank[targetIdx];
    g.signalDisturb = kDisturbBank[disturbIdx];
    g.signalPlanEditsLeft = std::max(2, 7 - g.signalPhrasesSolved / 2);
    g.signalMicroNeeded = std::min(8, 5 + g.signalPhrasesSolved / 2);
    g.signalRoute = {{0, 0, 0, 0, 0, 0, 0, 0}};
    g.signalPhase = SignalPhase::Plan;
    g.signalExecBeat = 0;
    g.signalRunLane = 2;
    g.signalAcks = 0;
    g.signalAckedThisBeat = false;
    g.signalTraceLane = {{2, 2, 2, 2, 2, 2, 2, 2}};
    g.signalTraceAck = {{false, false, false, false, false, false, false, false}};
    g.signalJamOffset = 0;
    g.signalWindowHitThisBeat = false;
}

void DrawTestCardCountdown(SDL_Renderer* renderer, const GameState& g) {
    const float t = g.songSeconds * 1.9f;
    const float phase = std::fmod(t * 0.22f, 1.0f);
    const float strobe = 0.5f + 0.5f * std::sin(t * 10.0f);

    SDL_SetRenderDrawColor(renderer, 6, 6, 10, 255);
    SDL_Rect full{0, 0, kWindowW, kWindowH};
    SDL_RenderFillRect(renderer, &full);

    // Fullscreen psychedelic wash.
    for (int y = 0; y < kWindowH; y += 6) {
        const float wave = std::sin(t * 4.0f + y * 0.03f) * 0.22f + std::cos(t * 2.7f + y * 0.02f) * 0.14f;
        const RGB c = HsvToRgb(phase + y * 0.0019f + wave, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, static_cast<uint8_t>(36 + 90 * (0.6f + 0.4f * strobe)));
        SDL_Rect band{0, y, kWindowW, 5};
        SDL_RenderFillRect(renderer, &band);
    }

    const int cardX = 84;
    const int cardY = 70;
    const int cardW = kWindowW - 168;
    const int cardH = kWindowH - 140;
    const RGB frameA = HsvToRgb(phase + 0.05f, 1.0f, 1.0f);
    const RGB frameB = HsvToRgb(phase + 0.35f, 1.0f, 1.0f);
    DrawPanel(renderer, cardX, cardY, cardW, cardH, {14, 14, 18}, frameA);
    SDL_SetRenderDrawColor(renderer, frameB.r, frameB.g, frameB.b, static_cast<uint8_t>(150 + 90 * strobe));
    SDL_Rect innerFrame{cardX + 10, cardY + 10, cardW - 20, cardH - 20};
    SDL_RenderDrawRect(renderer, &innerFrame);

    // TV-style color bars with animated hue offset.
    const int barY = cardY + 20;
    const int barH = 130;
    const int barCount = 14;
    const int barW = cardW / barCount;
    for (int i = 0; i < barCount; ++i) {
        const RGB c = HsvToRgb(phase + i * 0.065f + std::sin(t + i * 0.5f) * 0.03f, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255);
        SDL_Rect b{cardX + i * barW, barY, barW, barH};
        SDL_RenderFillRect(renderer, &b);
    }

    // Radial spokes.
    const int cx = kWindowW / 2;
    const int cy = kWindowH / 2 + 20;
    for (int i = 0; i < 32; ++i) {
        const float a = (static_cast<float>(i) / 32.0f) * kTau + t * 0.9f;
        const int x2 = cx + static_cast<int>(std::cos(a) * 360.0f);
        const int y2 = cy + static_cast<int>(std::sin(a) * 220.0f);
        const RGB c = HsvToRgb(phase + i * 0.03f, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, static_cast<uint8_t>(90 + 120 * strobe));
        SDL_RenderDrawLine(renderer, cx, cy, x2, y2);
    }

    // Concentric rings.
    for (int ring = 0; ring < 8; ++ring) {
        const float p = std::fmod(t * 0.42f + ring * 0.12f, 1.0f);
        const int rw = static_cast<int>(120 + p * 900);
        const int rh = static_cast<int>(80 + p * 500);
        const RGB c = HsvToRgb(phase + ring * 0.11f, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, static_cast<uint8_t>(120 - ring * 10));
        SDL_Rect rr{cx - rw / 2, cy - rh / 2, rw, rh};
        SDL_RenderDrawRect(renderer, &rr);
    }

    // Grayscale/check strip.
    const int grayY = barY + barH + 14;
    const int grayH = 42;
    for (int i = 0; i < 14; ++i) {
        const uint8_t v = static_cast<uint8_t>((i % 2 == 0) ? (i * 18) : (255 - i * 14));
        SDL_SetRenderDrawColor(renderer, v, v, v, 255);
        SDL_Rect gbar{cardX + i * (cardW / 14), grayY, cardW / 14, grayH};
        SDL_RenderFillRect(renderer, &gbar);
    }

    // Scanline and chroma split overlays.
    for (int y = 0; y < kWindowH; y += 2) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 30);
        SDL_RenderDrawLine(renderer, 0, y, kWindowW, y);
    }
    const RGB splitA = HsvToRgb(phase + 0.1f, 1.0f, 1.0f);
    const RGB splitB = HsvToRgb(phase + 0.6f, 1.0f, 1.0f);
    SDL_SetRenderDrawColor(renderer, splitA.r, splitA.g, splitA.b, 36);
    SDL_Rect leftTint{0, 0, kWindowW / 2, kWindowH};
    SDL_RenderFillRect(renderer, &leftTint);
    SDL_SetRenderDrawColor(renderer, splitB.r, splitB.g, splitB.b, 36);
    SDL_Rect rightTint{kWindowW / 2, 0, kWindowW / 2, kWindowH};
    SDL_RenderFillRect(renderer, &rightTint);

    const int display = std::max(0, static_cast<int>(std::ceil(g.intermissionCountdown)));
    const int countScale = 8 + static_cast<int>(strobe * 2.0f);
    const RGB digitColor = HsvToRgb(phase + 0.8f, 1.0f, 1.0f);
    DrawText(renderer, cardX + 30, cardY + cardH - 90, 2, {240, 240, 240}, "SIGNAL RELOCK");
    DrawText(renderer, cardX + 30, cardY + cardH - 60, 2, {190, 235, 255}, "NEXT GAME STARTS IN");
    DrawText(renderer, cx - 26, cy - 30, countScale, digitColor, std::to_string(display));
}

std::string TutorialDetail(const GameState& g) {
    switch (g.tutorialStep) {
        case TutorialStep::MoveMacro: return "P1 USE W A S D TO MOVE (" + std::to_string(g.tutorialMacroMoves) + "/3)";
        case TutorialStep::PlaceMacro: return "P1 PRESS SPACE TO PLACE 2 CELLS (" + std::to_string(g.tutorialMacroPlacements) + "/2)";
        case TutorialStep::MoveMicro: return "P2 USE ARROW KEYS TO MOVE (" + std::to_string(g.tutorialMicroMoves) + "/3)";
        case TutorialStep::PlaceMicro: return "P2 PRESS ENTER TO TOGGLE 4 CELLS (" + std::to_string(ActiveMicroCells(g)) + "/4)";
        case TutorialStep::PlayOneBar: return "LET THE PULSE RUN FOR ONE FULL BAR";
        case TutorialStep::Complete: return "BUILD RHYTHM + HARMONY TO SCORE";
    }
    return "";
}

void BeginNewBar(GameState& g) {
    g.prevChord = g.activeChord;
    g.activeChord = g.macroChordByBar[g.currentBar % kMacroCols];
    if (g.activeChord < 1 || g.activeChord > 7) g.activeChord = g.prevChord;
}

void AdvanceTutorial(GameState& g) {
    if (!g.tutorialEnabled || g.tutorialStep == TutorialStep::Complete) return;
    bool done = false;
    switch (g.tutorialStep) {
        case TutorialStep::MoveMacro: done = g.tutorialMacroMoves >= 3; break;
        case TutorialStep::PlaceMacro: done = g.tutorialMacroPlacements >= 2; break;
        case TutorialStep::MoveMicro: done = g.tutorialMicroMoves >= 3; break;
        case TutorialStep::PlaceMicro: done = ActiveMicroCells(g) >= 4 || g.tutorialMicroPlacements >= 4; break;
        case TutorialStep::PlayOneBar: done = g.currentBar > g.tutorialBarStart; break;
        case TutorialStep::Complete: break;
    }
    if (!done) return;
    if (g.tutorialStep == TutorialStep::PlayOneBar) { g.tutorialStep = TutorialStep::Complete; return; }
    g.tutorialStep = static_cast<TutorialStep>(static_cast<int>(g.tutorialStep) + 1);
    if (g.tutorialStep == TutorialStep::PlayOneBar) g.tutorialBarStart = g.currentBar;
}

int ObjectiveScore(const GameState& g) {
    switch (g.objective) {
        case Objective::LockGroove: return (g.micro[0][0] && g.micro[0][8]) ? 100 : 20;
        case Objective::Cadence: return (g.prevChord == 5 && g.activeChord == 1) ? 100 : 20;
        case Objective::SparseBar: return ActiveMicroCells(g) <= 6 ? 100 : 10;
        case Objective::SyncHit: {
            uint32_t diff = g.lastMacroCommitTicks > g.lastMicroCommitTicks ? g.lastMacroCommitTicks - g.lastMicroCommitTicks : g.lastMicroCommitTicks - g.lastMacroCommitTicks;
            return diff <= 180 ? 100 : 25;
        }
    }
    return 0;
}

void ResolveBar(GameState& g) {
    const int comboBefore = g.combo;
    const float timingAvgErr = g.metrics.timingSamples > 0 ? g.metrics.timingErrSum / static_cast<float>(g.metrics.timingSamples) : 0.25f;
    const int timingScore = static_cast<int>(std::clamp(100.0f * (1.0f - timingAvgErr), 0.0f, 100.0f));
    const int harmonyScore = g.metrics.harmonyTotal > 0 ? static_cast<int>(100.0f * g.metrics.harmonyHits / g.metrics.harmonyTotal) : 50;
    int grooveScore = 20;
    if (g.metrics.triggeredKick >= 2) grooveScore += 35;
    if (g.metrics.triggeredSnare >= 2) grooveScore += 25;
    if (g.metrics.triggeredHat >= 4) grooveScore += 20;
    grooveScore = std::clamp(grooveScore, 0, 100);

    const int objectiveScore = ObjectiveScore(g);
    const float barScore = 0.35f * timingScore + 0.35f * harmonyScore + 0.20f * grooveScore + 0.10f * objectiveScore;
    if (barScore >= 75.0f) g.combo += 1;
    else if (barScore < 50.0f) g.combo = 0;

    const float mult = 1.0f + static_cast<float>(std::min(g.combo, 20)) * 0.05f;
    g.score += static_cast<int>(barScore * mult);
    if (g.combo > comboBefore) { g.comboPulse = 1.0f; g.uiFlash = std::min(1.0f, g.uiFlash + 0.25f); }

    g.metrics = {};
    g.objectiveBarCounter++;
    if (g.objectiveBarCounter % 4 == 0) g.objective = static_cast<Objective>((g.objectiveBarCounter / 4) % 4);
}

void TriggerLane(JuceAudioEngine& audio, GameState& g, int lane, int step) {
    const auto chord = ChordForDegree(g.activeChord);
    if (lane == 0) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 120.0f, 0.7f, 11.0f, 0.25f);
        g.metrics.triggeredKick++;
        return;
    }
    if (lane == 1) {
        // Original snare body + extra high transient for snap.
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.4f, 35.0f, 0.10f);
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, 2400.0f, 0.09f, 130.0f, 0.012f);
        g.metrics.triggeredSnare++;
        return;
    }
    if (lane == 2) {
        TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.2f, 70.0f, 0.03f);
        g.metrics.triggeredHat++;
        return;
    }

    int semitone = 0;
    int midi = 60;
    if (lane == 3) { semitone = chord[0]; midi = 36 + semitone; }
    else if (lane == 4) { semitone = chord[(step / 4) % 3]; midi = 48 + semitone; }
    else if (lane == 5) { semitone = chord[step % 3]; midi = 60 + semitone; }
    else if (lane == 6) { semitone = chord[(step + 1) % 3]; midi = 72 + semitone; }
    else { semitone = (step * 2) % 12; midi = 67 + semitone; }

    g.metrics.harmonyTotal++;
    if (std::find(chord.begin(), chord.end(), semitone % 12) != chord.end() || InScale(semitone % 12)) g.metrics.harmonyHits++;

    TriggerVoice(audio,
                 JuceAudioEngine::VoiceType::Sine,
                 MidiToFreq(midi),
                 lane == 7 ? 0.20f : 0.25f,
                 lane == 6 ? 14.0f : 8.5f,
                 lane == 6 ? 0.08f : 0.18f);
}

void DrawPanel(SDL_Renderer* r, int x, int y, int w, int h, RGB fill, RGB border) {
    SDL_SetRenderDrawColor(r, fill.r, fill.g, fill.b, 255);
    SDL_Rect rect{x, y, w, h}; SDL_RenderFillRect(r, &rect);
    SDL_SetRenderDrawColor(r, 6, 10, 18, 255);
    SDL_Rect outer{x - 2, y - 2, w + 4, h + 4}; SDL_RenderDrawRect(r, &outer);
    SDL_SetRenderDrawColor(r, border.r, border.g, border.b, 255);
    SDL_RenderDrawRect(r, &rect);
    SDL_Rect inner{x + 1, y + 1, w - 2, h - 2}; SDL_RenderDrawRect(r, &inner);
}

void DrawRetroRoom(SDL_Renderer* r, int w, int h) {
    SDL_SetRenderDrawColor(r, 11, 20, 52, 255);
    SDL_Rect bg{0,0,w,h}; SDL_RenderFillRect(r, &bg);
    for (int y = 10; y < h - 10; y += 14) for (int x = 10; x < w - 10; x += 14) {
        if (((x * 37 + y * 53) % 101) < 16) {
            SDL_SetRenderDrawColor(r, 104, 132, 255, 255);
            SDL_RenderDrawPoint(r, x, y);
        }
    }
    SDL_SetRenderDrawColor(r, 5, 8, 18, 70);
    for (int y = 0; y < h; y += 2) SDL_RenderDrawLine(r, 0, y, w, y);

    const int brickW = 16, brickH = 8, t = 26;
    const RGB a{82,146,238}, b{57,103,187};
    for (int yy = 0; yy < t; yy += brickH) for (int xx = 0; xx < w; xx += brickW) {
        const RGB c = (((xx / brickW) + (yy / brickH)) % 2 == 0) ? a : b;
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
        SDL_Rect r1{xx,yy,brickW-1,brickH-1}; SDL_RenderFillRect(r, &r1);
        SDL_Rect r2{xx,h-t+yy,brickW-1,brickH-1}; SDL_RenderFillRect(r, &r2);
    }
    for (int xx = 0; xx < t; xx += brickH) for (int yy = t; yy < h - t; yy += brickW) {
        const RGB c = (((xx / brickH) + (yy / brickW)) % 2 == 0) ? a : b;
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
        SDL_Rect r1{xx,yy,brickH-1,brickW-1}; SDL_RenderFillRect(r, &r1);
        SDL_Rect r2{w-t+xx,yy,brickH-1,brickW-1}; SDL_RenderFillRect(r, &r2);
    }
}

void DrawPsychedelicWash(SDL_Renderer* r, float t, float intensity) {
    const float i = std::clamp(intensity, 0.0f, 1.0f);
    for (int y = 0; y < kWindowH; y += 8) {
        const float wave = std::sin(t * 3.8f + y * 0.045f) * 0.18f + std::cos(t * 1.9f + y * 0.02f) * 0.08f;
        const RGB c = HsvToRgb(0.24f * t + y * 0.0019f + wave, 0.96f, 1.0f);
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, static_cast<uint8_t>(30 + 88 * i));
        SDL_Rect band{0, y, kWindowW, 7};
        SDL_RenderFillRect(r, &band);
    }
}

void DrawNeonBeatRings(SDL_Renderer* r, float t, float beatPulse, float intensity) {
    const float i = std::clamp(intensity, 0.0f, 1.0f);
    const int cx = kWindowW / 2;
    const int cy = kWindowH / 2;
    for (int n = 0; n < 8; ++n) {
        const float phase = std::fmod(t * 0.9f + n * 0.13f, 1.0f);
        const float p = std::fmod(phase + (1.0f - beatPulse) * 0.2f, 1.0f);
        const int w = static_cast<int>(160 + p * 1240);
        const int h = static_cast<int>(90 + p * 640);
        const RGB c = HsvToRgb(p + t * 0.42f + n * 0.11f, 0.98f, 1.0f);
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, static_cast<uint8_t>((66 - n * 6) * i));
        SDL_Rect ring{cx - w / 2, cy - h / 2, w, h};
        SDL_RenderDrawRect(r, &ring);
    }
}

void DrawGrid(SDL_Renderer* r, int x, int y, int rows, int cols, int cellW, int cellH,
              const std::function<bool(int,int)>& activeFn, int cursorRow, int cursorCol,
              int pulseCol, float pulseStrength, float rainbowPhase, RGB base, RGB active, RGB pulse) {
    const float p = std::clamp(pulseStrength, 0.0f, 1.0f);
    auto wrappedDist = [cols](int a, int b) {
        const int d = std::abs(a - b);
        return std::min(d, cols - d);
    };
    for (int rr = 0; rr < rows; ++rr) for (int cc = 0; cc < cols; ++cc) {
        SDL_Rect cell{x + cc * cellW, y + rr * cellH, cellW - 2, cellH - 2};
        SDL_SetRenderDrawColor(r, base.r, base.g, base.b, 255);
        SDL_RenderFillRect(r, &cell);

        const int trailDist = wrappedDist(cc, pulseCol);
        if (trailDist <= 3) {
            const float t = 1.0f - static_cast<float>(trailDist) / 3.0f;
            const uint8_t alpha = static_cast<uint8_t>((trailDist == 0 ? 50 : 22) + (trailDist == 0 ? 165.0f : 72.0f) * t * p);
            SDL_SetRenderDrawColor(r, pulse.r, pulse.g, pulse.b, alpha);
            SDL_RenderFillRect(r, &cell);
        }

        if (activeFn(rr, cc)) {
            SDL_SetRenderDrawColor(r, active.r, active.g, active.b, 255);
            SDL_Rect inset{cell.x + 3, cell.y + 3, cell.w - 6, cell.h - 6};
            SDL_RenderFillRect(r, &inset);
            const RGB neon = HsvToRgb(rainbowPhase + rr * 0.065f + cc * 0.04f, 0.95f, 1.0f);
            SDL_SetRenderDrawColor(r, neon.r, neon.g, neon.b, 210);
            SDL_Rect neonFill{inset.x + 1, inset.y + 1, inset.w - 2, inset.h - 2};
            SDL_RenderFillRect(r, &neonFill);
            SDL_SetRenderDrawColor(r, neon.r, neon.g, neon.b, 255);
            SDL_Rect glow{inset.x + 1, inset.y + 1, inset.w - 2, inset.h - 2};
            SDL_RenderDrawRect(r, &glow);
            SDL_SetRenderDrawColor(r, 255, 255, 255, 120);
            SDL_Rect shine{inset.x, inset.y, inset.w, 2};
            SDL_RenderFillRect(r, &shine);
        }

        if (cc == pulseCol) {
            SDL_SetRenderDrawColor(r, pulse.r, pulse.g, pulse.b, 255);
            SDL_RenderDrawRect(r, &cell);
        }
        if (rr == cursorRow && cc == cursorCol) {
            SDL_SetRenderDrawColor(r, 245, 245, 245, 255);
            SDL_Rect c2{cell.x + 1, cell.y + 1, cell.w - 2, cell.h - 2};
            SDL_RenderDrawRect(r, &c2);
        }
    }
}

RGB LaneColor(int lane) {
    switch (lane) {
        case 0: return {255, 196, 88};
        case 1: return {255, 120, 120};
        case 2: return {180, 230, 255};
        case 3: return {255, 160, 92};
        case 4: return {255, 220, 110};
        case 5: return {110, 230, 255};
        case 6: return {196, 164, 255};
        default: return {120, 255, 190};
    }
}

void SpawnBurst(GameState& g, float x, float y, RGB color, int count) {
    std::uniform_real_distribution<float> angDist(0.0f, kTau);
    std::uniform_real_distribution<float> speedDist(40.0f, 170.0f);
    std::uniform_real_distribution<float> lifeDist(0.18f, 0.42f);
    std::uniform_real_distribution<float> jitter(-4.0f, 4.0f);

    for (int i = 0; i < count; ++i) {
        const float a = angDist(g.visualRng);
        const float s = speedDist(g.visualRng);
        Particle p;
        p.x = x + jitter(g.visualRng);
        p.y = y + jitter(g.visualRng);
        p.vx = std::cos(a) * s;
        p.vy = std::sin(a) * s - 30.0f;
        p.life = lifeDist(g.visualRng);
        p.maxLife = p.life;
        p.color = color;
        g.particles.push_back(p);
    }
}

void SpawnBigExplosion(GameState& g, float x, float y, RGB core, RGB outer) {
    std::uniform_real_distribution<float> angDist(0.0f, kTau);
    std::uniform_real_distribution<float> speedDist(70.0f, 340.0f);
    std::uniform_real_distribution<float> lifeDist(0.45f, 1.25f);
    std::uniform_real_distribution<float> jitter(-10.0f, 10.0f);

    for (int i = 0; i < 280; ++i) {
        const float a = angDist(g.visualRng);
        const float s = speedDist(g.visualRng);
        Particle p;
        p.x = x + jitter(g.visualRng);
        p.y = y + jitter(g.visualRng);
        p.vx = std::cos(a) * s;
        p.vy = std::sin(a) * s - 28.0f;
        p.life = lifeDist(g.visualRng);
        p.maxLife = p.life;
        p.color = (i % 3 == 0) ? outer : core;
        g.particles.push_back(p);
    }
}

void UpdateParticles(GameState& g, float dt) {
    for (auto& p : g.particles) {
        p.life -= dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.vy += 220.0f * dt;
        p.vx *= 0.985f;
    }
    g.particles.erase(std::remove_if(g.particles.begin(), g.particles.end(),
                                     [](const Particle& p) { return p.life <= 0.0f; }),
                      g.particles.end());
}

void DrawParticles(SDL_Renderer* r, const GameState& g) {
    for (const auto& p : g.particles) {
        const float t = std::clamp(p.life / std::max(0.0001f, p.maxLife), 0.0f, 1.0f);
        const uint8_t alpha = static_cast<uint8_t>(t * 220.0f);
        const int size = (t > 0.6f) ? 4 : ((t > 0.3f) ? 3 : 2);
        SDL_SetRenderDrawColor(r, p.color.r, p.color.g, p.color.b, alpha);
        SDL_Rect px{static_cast<int>(std::round(p.x)), static_cast<int>(std::round(p.y)), size, size};
        SDL_RenderFillRect(r, &px);
    }
}

void DrawRailGridLockedParticles(SDL_Renderer* r, const GameState& g) {
    for (const auto& p : g.particles) {
        const float t = std::clamp(p.life / std::max(0.0001f, p.maxLife), 0.0f, 1.0f);
        const uint8_t alpha = static_cast<uint8_t>(t * 220.0f);
        const int size = (t > 0.6f) ? 5 : ((t > 0.3f) ? 4 : 3);
        const int snapCell = RailSnapCell(p.x);
        const int snapLane = RailSnapLane(p.y);
        const int x = static_cast<int>(std::lround(RailCellX(static_cast<float>(snapCell))));
        const int y = RailLaneCenterY(snapLane);

        SDL_SetRenderDrawColor(r, p.color.r, p.color.g, p.color.b, alpha);
        SDL_Rect px{x - size / 2, y - size / 2, size, size};
        SDL_RenderFillRect(r, &px);
    }
}

} // namespace

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("Multiplayer Music (Couch Co-op)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                           kWindowW, kWindowH, SDL_WINDOW_SHOWN);
    if (!window) { std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) { std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError()); SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    JuceAudioEngine audio;
    if (!audio.start()) {
        std::fprintf(stderr, "Failed to start JUCE audio engine.\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    GameState g;
    const uint32_t startTicks = SDL_GetTicks();
    g.lastFrameTicks = startTicks;

    InitTitleScreen(g, startTicks);

    while (g.running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                g.running = false;
            } else if (ev.type == SDL_KEYDOWN && ev.key.repeat == 0) {
                const SDL_Keycode key = ev.key.keysym.sym;
                if (key == SDLK_ESCAPE) g.running = false;
                else if (key == SDLK_p) { g.paused = !g.paused; g.lastFrameTicks = SDL_GetTicks(); }
                else if (key == SDLK_t) g.tutorialEnabled = !g.tutorialEnabled;
                if (g.paused) continue;

                if (g.mode == GameMode::TestCardCooldown) {
                    continue;
                }

                if (g.mode == GameMode::TitleScreen) {
                    if (key == SDLK_UP || key == SDLK_w) g.titleSelection = (g.titleSelection + 6) % 7;
                    else if (key == SDLK_DOWN || key == SDLK_s) g.titleSelection = (g.titleSelection + 1) % 7;
                    else if (key == SDLK_1) g.titleSelection = 0;
                    else if (key == SDLK_2) g.titleSelection = 1;
                    else if (key == SDLK_3) g.titleSelection = 2;
                    else if (key == SDLK_4) g.titleSelection = 3;
                    else if (key == SDLK_5) g.titleSelection = 4;
                    else if (key == SDLK_6) g.titleSelection = 5;
                    else if (key == SDLK_7) g.titleSelection = 6;
                    else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
                        if (g.titleSelection == 0) InitGridCoopMode(g, SDL_GetTicks());
                        else if (g.titleSelection == 1) InitDuelMode(g, SDL_GetTicks());
                        else if (g.titleSelection == 2) InitRailMode(g, SDL_GetTicks());
                        else if (g.titleSelection == 3) InitSignalMode(g, SDL_GetTicks());
                        else if (g.titleSelection == 4) InitNuclearMode(g, SDL_GetTicks());
                        else if (g.titleSelection == 5) InitSnakeMode(g, SDL_GetTicks());
                        else InitLongJumpMode(g, SDL_GetTicks());
                    }
                    continue;
                }

                if (g.mode == GameMode::LongJumpDuet) {
                    const float beatDur = 60.0f / g.longJumpBpm;
                    const float secInBeat = std::fmod(static_cast<float>(SDL_GetTicks() - g.longJumpBeatStartTicks) / 1000.0f, beatDur);
                    const float distToBeat = std::min(secInBeat, beatDur - secInBeat);
                    const bool onBeat = distToBeat <= 0.100f;

                    if (key == SDLK_a || key == SDLK_d) {
                        const int foot = (key == SDLK_a) ? 0 : 1;
                        g.ljLastFoot1 = foot;
                        if (!g.ljJumped1 && !g.ljDone1) {
                            if (onBeat && foot == g.ljExpectedFoot1) {
                                g.ljCombo1 = std::min(32, g.ljCombo1 + 1);
                                g.ljExpectedFoot1 = 1 - g.ljExpectedFoot1;
                                g.ljSpeed1 = std::min(14.0f, g.ljSpeed1 + 0.65f + g.ljCombo1 * 0.03f);
                                g.ljFootGlow1[foot] = 1.0f;
                                TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 100.0f + g.ljCombo1 * 2.0f, 0.28f, 12.0f, 0.11f);
                                SpawnBurst(g, 250.0f + g.ljRun1 * 18.0f, 430.0f, {255, 220, 120}, 8);
                            } else {
                                g.ljCombo1 = 0;
                                g.ljSpeed1 = std::max(3.0f, g.ljSpeed1 - 0.9f);
                                g.ljFootGlow1[foot] = std::max(g.ljFootGlow1[foot], 0.45f);
                                TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.06f, 140.0f, 0.03f);
                            }
                        }
                    } else if (key == SDLK_w) {
                        g.ljAngle1 = std::clamp(g.ljAngle1 + 1.0f, 16.0f, 52.0f);
                    } else if (key == SDLK_s) {
                        g.ljAngle1 = std::clamp(g.ljAngle1 - 1.0f, 16.0f, 52.0f);
                    } else if (key == SDLK_SPACE) {
                        if (!g.ljJumped1 && !g.ljDone1) {
                            g.ljJumped1 = true;
                            const float takeoff = 18.0f;
                            if (g.ljRun1 > takeoff + 0.30f) {
                                g.ljFoul1 = true;
                                g.ljDone1 = true;
                            } else {
                                const float rad = g.ljAngle1 * 3.14159265f / 180.0f;
                                g.ljVx1 = g.ljSpeed1 * std::cos(rad) * 2.10f;
                                g.ljVy1 = g.ljSpeed1 * std::sin(rad) * 1.44f;
                                g.ljInAir1 = true;
                                g.ljFlightX1 = 0.0f;
                                g.ljFlightY1 = 0.0f;
                            }
                            TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(72), 0.11f, 20.0f, 0.08f);
                        }
                    }

                    if (key == SDLK_LEFT || key == SDLK_RIGHT) {
                        const int foot = (key == SDLK_LEFT) ? 0 : 1;
                        g.ljLastFoot2 = foot;
                        if (!g.ljJumped2 && !g.ljDone2) {
                            if (onBeat && foot == g.ljExpectedFoot2) {
                                g.ljCombo2 = std::min(32, g.ljCombo2 + 1);
                                g.ljExpectedFoot2 = 1 - g.ljExpectedFoot2;
                                g.ljSpeed2 = std::min(14.0f, g.ljSpeed2 + 0.65f + g.ljCombo2 * 0.03f);
                                g.ljFootGlow2[foot] = 1.0f;
                                TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 110.0f + g.ljCombo2 * 2.0f, 0.28f, 12.0f, 0.11f);
                                SpawnBurst(g, 770.0f + g.ljRun2 * 18.0f, 430.0f, {120, 230, 255}, 8);
                            } else {
                                g.ljCombo2 = 0;
                                g.ljSpeed2 = std::max(3.0f, g.ljSpeed2 - 0.9f);
                                g.ljFootGlow2[foot] = std::max(g.ljFootGlow2[foot], 0.45f);
                                TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.06f, 140.0f, 0.03f);
                            }
                        }
                    } else if (key == SDLK_UP) {
                        g.ljAngle2 = std::clamp(g.ljAngle2 + 1.0f, 16.0f, 52.0f);
                    } else if (key == SDLK_DOWN) {
                        g.ljAngle2 = std::clamp(g.ljAngle2 - 1.0f, 16.0f, 52.0f);
                    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                        if (!g.ljJumped2 && !g.ljDone2) {
                            g.ljJumped2 = true;
                            const float takeoff = 18.0f;
                            if (g.ljRun2 > takeoff + 0.30f) {
                                g.ljFoul2 = true;
                                g.ljDone2 = true;
                            } else {
                                const float rad = g.ljAngle2 * 3.14159265f / 180.0f;
                                g.ljVx2 = g.ljSpeed2 * std::cos(rad) * 2.10f;
                                g.ljVy2 = g.ljSpeed2 * std::sin(rad) * 1.44f;
                                g.ljInAir2 = true;
                                g.ljFlightX2 = 0.0f;
                                g.ljFlightY2 = 0.0f;
                            }
                            TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(76), 0.11f, 20.0f, 0.08f);
                        }
                    }
                    continue;
                }

                if (g.mode == GameMode::SnakeDuet) {
                    // P1 snake (WASD)
                    if (key == SDLK_w && g.snakeDir1 != 2) g.snakePendingDir1 = 0;
                    else if (key == SDLK_d && g.snakeDir1 != 3) g.snakePendingDir1 = 1;
                    else if (key == SDLK_s && g.snakeDir1 != 0) g.snakePendingDir1 = 2;
                    else if (key == SDLK_a && g.snakeDir1 != 1) g.snakePendingDir1 = 3;
                    // P2 snake (arrows)
                    else if (key == SDLK_UP && g.snakeDir2 != 2) g.snakePendingDir2 = 0;
                    else if (key == SDLK_RIGHT && g.snakeDir2 != 3) g.snakePendingDir2 = 1;
                    else if (key == SDLK_DOWN && g.snakeDir2 != 0) g.snakePendingDir2 = 2;
                    else if (key == SDLK_LEFT && g.snakeDir2 != 1) g.snakePendingDir2 = 3;
                    continue;
                }

                if (g.mode == GameMode::NuclearRhythmWar) {
                    const float beatDur = 60.0f / g.nuclearBpm;
                    const float secInBeat = std::fmod(static_cast<float>(SDL_GetTicks() - g.nuclearBeatStartTicks) / 1000.0f, beatDur);
                    const float distToBeat = std::min(secInBeat, beatDur - secInBeat);
                    const bool onBeat = distToBeat <= 0.108f;

                    // P1 macro doctrine programming.
                    if (key == SDLK_a) g.nuclearMacroCursor = (g.nuclearMacroCursor + 7) % 8;
                    else if (key == SDLK_d) g.nuclearMacroCursor = (g.nuclearMacroCursor + 1) % 8;
                    else if (key == SDLK_w) g.nuclearDoctrine[g.nuclearMacroCursor] = std::min(3, g.nuclearDoctrine[g.nuclearMacroCursor] + 1);
                    else if (key == SDLK_s) g.nuclearDoctrine[g.nuclearMacroCursor] = std::max(0, g.nuclearDoctrine[g.nuclearMacroCursor] - 1);
                    else if (key == SDLK_BACKSPACE) g.nuclearDoctrine = {{0, 0, 0, 0, 0, 0, 0, 0}};

                    // P2 micro interception.
                    if (key == SDLK_LEFT) g.nuclearMicroCursor = std::max(0, g.nuclearMicroCursor - 1);
                    else if (key == SDLK_RIGHT) g.nuclearMicroCursor = std::min(7, g.nuclearMicroCursor + 1);
                    else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                        if (onBeat) {
                            g.nuclearIntercept[g.nuclearMicroCursor] = true;
                        } else {
                            g.nuclearDevastation = std::min(100, g.nuclearDevastation + 2);
                            g.nuclearStability = std::max(0, g.nuclearStability - 2);
                            g.nuclearFlash = 1.0f;
                        }
                    }
                    continue;
                }

                if (g.mode == GameMode::SignalForge) {
                    const float beatDur = 60.0f / g.signalBpm;
                    const float secInBeat = std::fmod(static_cast<float>(SDL_GetTicks() - g.signalBeatStartTicks) / 1000.0f, beatDur);
                    const float distToBeat = std::min(secInBeat, beatDur - secInBeat);
                    const float tuneWindow = std::max(0.040f, 0.10f - g.signalInterference * 0.03f);
                    const bool onWindow = distToBeat <= tuneWindow;

                    // P1 builder: full grid cursor + rotate.
                    if (key == SDLK_w) g.signalP1Row = std::max(0, g.signalP1Row - 1);
                    else if (key == SDLK_s) g.signalP1Row = std::min(kSignalRows - 1, g.signalP1Row + 1);
                    else if (key == SDLK_a) g.signalP1Col = std::max(0, g.signalP1Col - 1);
                    else if (key == SDLK_d) g.signalP1Col = std::min(kSignalCols - 1, g.signalP1Col + 1);
                    else if (key == SDLK_SPACE) {
                        g.signalTileRot[g.signalP1Row][g.signalP1Col] = (g.signalTileRot[g.signalP1Row][g.signalP1Col] + 1) & 3;
                        if (!onWindow) g.signalInterference = std::min(1.0f, g.signalInterference + 0.01f);
                    }

                    // P2 tuner: spectrum cursor + lock attempt.
                    if (key == SDLK_LEFT) g.signalTuneCursor = std::max(0, g.signalTuneCursor - 1);
                    else if (key == SDLK_RIGHT) g.signalTuneCursor = std::min(15, g.signalTuneCursor + 1);
                    else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                        const float tolerance = 0.7f + static_cast<float>(g.signalPower) * 0.035f;  // easier as radio powers up
                        const float dist = std::abs(static_cast<float>(g.signalTuneCursor) - g.signalTargetFreq);
                        if (onWindow && dist <= tolerance) {
                            g.signalTuneStreak++;
                            g.signalTuneLock = std::min(100, g.signalTuneLock + 4 + g.signalPower / 12);
                            g.signalClean += 3;
                            g.signalCombo++;
                            g.signalInterference = std::max(0.0f, g.signalInterference - 0.02f);
                        } else {
                            g.signalTuneStreak = 0;
                            g.signalTuneLock = std::max(0, g.signalTuneLock - 7);
                            g.signalNoise += 2;
                            g.signalCombo = 0;
                            g.signalInterference = std::min(1.0f, g.signalInterference + 0.03f);
                        }
                    } else if (key == SDLK_BACKSPACE) {
                        GenerateSignalCircuit(g);
                    }
                    continue;
                }

                if (g.mode == GameMode::RailSignalRush) {
                    const float stepDur = 60.0f / g.railBpm / 4.0f;
                    const float secInStep = std::fmod(static_cast<float>(SDL_GetTicks() - g.railStepStartTicks) / 1000.0f, stepDur);
                    const float distToStep = std::min(secInStep, stepDur - secInStep);
                    const bool onTick = distToStep <= kRailOnTickWindowSec;

                    // P2 (macro): A/D move, W=UP, S=STRAIGHT
                    if (key == SDLK_a) {
                        g.railMacroCursor = (g.railMacroCursor + 3) % 4;
                    } else if (key == SDLK_d) {
                        g.railMacroCursor = (g.railMacroCursor + 1) % 4;
                    } else if (key == SDLK_w) {
                        g.railMacroDelta[g.railMacroCursor] = -1;
                    } else if (key == SDLK_s) {
                        g.railMacroDelta[g.railMacroCursor] = 0;
                    // P1 (micro): arrows move/select + toggle FLIP/NORM
                    } else if (key == SDLK_LEFT) {
                        g.railMicroCursor = (g.railMicroCursor + 3) % 4;
                    } else if (key == SDLK_RIGHT) {
                        g.railMicroCursor = (g.railMicroCursor + 1) % 4;
                    } else if (key == SDLK_UP) {
                        if (onTick) g.railMicroFlip[g.railMicroCursor] = !g.railMicroFlip[g.railMicroCursor];
                        else g.railFlash = std::max(g.railFlash, 0.5f);
                    } else if (key == SDLK_DOWN) {
                        if (onTick) g.railMicroFlip[g.railMicroCursor] = !g.railMicroFlip[g.railMicroCursor];
                        else g.railFlash = std::max(g.railFlash, 0.5f);
                    }
                    continue;
                }

                if (g.mode == GameMode::DuelArena) {
                    const float beatDur = 60.0f / g.duelBpm;
                    const float secInBeat = std::fmod(static_cast<float>(SDL_GetTicks() - g.duelBeatStartTicks) / 1000.0f, beatDur);
                    const int q = BeatQuality(secInBeat, beatDur);
                    const float distToBeat = std::min(secInBeat, beatDur - secInBeat);
                    const bool onBeat = distToBeat <= kDuelOnBeatWindowSec;

                    if (key == SDLK_a) {
                        if (onBeat) { g.p1Queued = DuelAction::MoveLeft; g.p1Quality = q; }
                        else { g.p1Queued = DuelAction::None; g.p1Quality = 0; g.p1Meter = std::max(0, g.p1Meter - 2); g.p1Flash = 0.9f; }
                    } else if (key == SDLK_d) {
                        if (onBeat) { g.p1Queued = DuelAction::MoveRight; g.p1Quality = q; }
                        else { g.p1Queued = DuelAction::None; g.p1Quality = 0; g.p1Meter = std::max(0, g.p1Meter - 2); g.p1Flash = 0.9f; }
                    } else if (key == SDLK_w) {
                        if (onBeat) { g.p1Queued = DuelAction::Attack; g.p1Quality = q; }
                        else { g.p1Queued = DuelAction::None; g.p1Quality = 0; g.p1Meter = std::max(0, g.p1Meter - 4); g.p1Flash = 1.0f; }
                    }
                    else if (key == SDLK_s) { g.p1Queued = DuelAction::Block; g.p1Quality = q; }
                    else if (key == SDLK_LEFT) {
                        if (onBeat) { g.p2Queued = DuelAction::MoveLeft; g.p2Quality = q; }
                        else { g.p2Queued = DuelAction::None; g.p2Quality = 0; g.p2Meter = std::max(0, g.p2Meter - 2); g.p2Flash = 0.9f; }
                    } else if (key == SDLK_RIGHT) {
                        if (onBeat) { g.p2Queued = DuelAction::MoveRight; g.p2Quality = q; }
                        else { g.p2Queued = DuelAction::None; g.p2Quality = 0; g.p2Meter = std::max(0, g.p2Meter - 2); g.p2Flash = 0.9f; }
                    } else if (key == SDLK_UP) {
                        if (onBeat) { g.p2Queued = DuelAction::Attack; g.p2Quality = q; }
                        else { g.p2Queued = DuelAction::None; g.p2Quality = 0; g.p2Meter = std::max(0, g.p2Meter - 4); g.p2Flash = 1.0f; }
                    }
                    else if (key == SDLK_DOWN) { g.p2Queued = DuelAction::Block; g.p2Quality = q; }
                    continue;
                }

                const uint32_t now = SDL_GetTicks();
                const float stepDur = StepDurationSeconds(g.bpm);
                const float secInBar = static_cast<float>(now - g.barStartTicks) / 1000.0f;
                const float nearestStep = std::round(secInBar / stepDur) * stepDur;
                const float err = std::min(1.0f, std::abs(secInBar - nearestStep) / (stepDur * 0.5f));

                if (key == SDLK_w) { g.macroSelectedRow = (g.macroSelectedRow + kMacroRows - 1) % kMacroRows; g.tutorialMacroMoves++; }
                else if (key == SDLK_s) { g.macroSelectedRow = (g.macroSelectedRow + 1) % kMacroRows; g.tutorialMacroMoves++; }
                else if (key == SDLK_a) { g.macroCursorCol = (g.macroCursorCol + kMacroCols - 1) % kMacroCols; g.tutorialMacroMoves++; }
                else if (key == SDLK_d) { g.macroCursorCol = (g.macroCursorCol + 1) % kMacroCols; g.tutorialMacroMoves++; }
                else if (key == SDLK_SPACE) {
                    g.macroChordByBar[g.macroCursorCol] = g.macroSelectedRow + 1;
                    g.tutorialMacroPlacements++;
                    g.lastMacroCommitTicks = now;
                    g.metrics.timingErrSum += err;
                    g.metrics.timingSamples++;
                    g.uiFlash = std::min(1.0f, g.uiFlash + 0.18f);
                } else if (key == SDLK_LEFT) { g.microCursorCol = (g.microCursorCol + kMicroCols - 1) % kMicroCols; g.tutorialMicroMoves++; }
                else if (key == SDLK_RIGHT) { g.microCursorCol = (g.microCursorCol + 1) % kMicroCols; g.tutorialMicroMoves++; }
                else if (key == SDLK_UP) { g.microSelectedRow = (g.microSelectedRow + kMicroRows - 1) % kMicroRows; g.tutorialMicroMoves++; }
                else if (key == SDLK_DOWN) { g.microSelectedRow = (g.microSelectedRow + 1) % kMicroRows; g.tutorialMicroMoves++; }
                else if (key == SDLK_RETURN) {
                    bool& cell = g.micro[g.microSelectedRow][g.microCursorCol];
                    cell = !cell;
                    g.tutorialMicroPlacements++;
                    g.lastMicroCommitTicks = now;
                    g.metrics.timingErrSum += err;
                    g.metrics.timingSamples++;
                    g.uiFlash = std::min(1.0f, g.uiFlash + 0.12f);
                } else if (key == SDLK_BACKSPACE) {
                    for (int r = 0; r < kMicroRows; ++r) for (int c = 0; c < kMicroCols; ++c) g.micro[r][c] = false;
                } else if (key == SDLK_TAB) {
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, 440.0f, 0.1f, 8.0f, 0.1f);
                }
                AdvanceTutorial(g);
            }
        }

        const uint32_t now = SDL_GetTicks();
        float dt = static_cast<float>(now - g.lastFrameTicks) / 1000.0f;
        g.lastFrameTicks = now;

        if (g.paused) {
            dt = 0.0f;
            if (g.mode == GameMode::GridCoop) {
                g.barStartTicks = now - static_cast<uint32_t>(static_cast<float>(g.currentStep) * StepDurationSeconds(g.bpm) * 1000.0f);
            } else if (g.mode == GameMode::SnakeDuet) {
                g.snakeBeatStartTicks = now;
                g.snakeStepStartTicks = now;
            } else if (g.mode == GameMode::LongJumpDuet) {
                g.longJumpBeatStartTicks = now;
                g.longJumpStepStartTicks = now;
            } else if (g.mode == GameMode::NuclearRhythmWar) {
                g.nuclearBeatStartTicks = now;
                g.nuclearStepStartTicks = now;
            } else if (g.mode == GameMode::SignalForge) {
                g.signalBeatStartTicks = now;
                g.signalStepStartTicks = now;
            } else if (g.mode == GameMode::RailSignalRush) {
                g.railBeatStartTicks = now;
                g.railStepStartTicks = now;
            } else {
                g.duelBeatStartTicks = now;
            }
        } else {
            g.songSeconds += dt;
        }

        if (g.mode == GameMode::TitleScreen) {
            DrawRetroRoom(renderer, kWindowW, kWindowH);
            const float ph = std::fmod(g.songSeconds * 0.18f, 1.0f);
            DrawPsychedelicWash(renderer, g.songSeconds, 0.58f);
            DrawNeonBeatRings(renderer, g.songSeconds, 0.35f + 0.25f * std::sin(g.songSeconds * 2.0f), 0.55f);

            const int panelX = 170, panelY = 110, panelW = kWindowW - 340, panelH = kWindowH - 220;
            DrawPanel(renderer, panelX, panelY, panelW, panelH, {12, 20, 44}, HsvToRgb(ph + 0.08f, 1.0f, 1.0f));
            DrawText(renderer, panelX + 30, panelY + 26, 4, {245, 248, 255}, "MULTIPLAYER MUSIC");
            DrawText(renderer, panelX + 30, panelY + 70, 2, {210, 224, 255}, "SELECT STARTING GAME");

            const std::array<std::string, 7> marqueeNames = {{
                "GRID CO-OP",
                "CHORD DUEL ARENA",
                "RAIL SIGNAL RUSH",
                "SIGNAL FORGE",
                "STRANGELOVE",
                "DOUBLE SNAKE",
                "JEDWARD'S LONGJUMP"
            }};

            const int jukeboxX = panelX + 30;
            const int jukeboxY = panelY + 104;
            const int jukeboxW = panelW - 60;
            const int jukeboxH = 38;
            DrawPanel(renderer, jukeboxX, jukeboxY, jukeboxW, jukeboxH, {14, 14, 24}, {255, 170, 120});
            DrawText(renderer, jukeboxX + 10, jukeboxY + 11, 1, {255, 170, 120}, "JUKEBOX:");

            const std::string track = "NOW SELECTED > " + marqueeNames[std::clamp(g.titleSelection, 0, 6)] + " <";
            const int trackPx = static_cast<int>(track.size()) * 12; // 6px font * scale 2
            const int loopPx = trackPx + jukeboxW - 120;
            const int scroll = static_cast<int>(std::fmod(g.songSeconds * 90.0f, static_cast<float>(std::max(1, loopPx))));
            const int baseX = jukeboxX + jukeboxW - 16 - scroll;
            SDL_Rect clip{jukeboxX + 100, jukeboxY + 4, jukeboxW - 106, jukeboxH - 8};
            SDL_RenderSetClipRect(renderer, &clip);
            DrawText(renderer, baseX, jukeboxY + 10, 2, {255, 236, 140}, track);
            DrawText(renderer, baseX + trackPx + 80, jukeboxY + 10, 2, {255, 236, 140}, track);
            SDL_RenderSetClipRect(renderer, nullptr);

            const std::array<std::string, 7> modes = {{
                "1. GRID CO-OP",
                "2. CHORD DUEL ARENA",
                "3. RAIL SIGNAL RUSH",
                "4. SIGNAL FORGE",
                "5. STRANGELOVE",
                "6. DOUBLE SNAKE",
                "7. JEDWARD'S LONGJUMP"
            }};

            // Scrollable menu viewport so list always fits as game count grows.
            const int listX = panelX + 30;
            const int listY = panelY + 152;
            const int listW = panelW - 60;
            const int listH = panelH - 230;
            const int rowH = 72;
            const int visibleRows = std::max(1, listH / rowH);
            const int maxStart = std::max(0, static_cast<int>(modes.size()) - visibleRows);
            const int scrollStart = std::clamp(g.titleSelection - visibleRows / 2, 0, maxStart);

            DrawPanel(renderer, listX, listY, listW, listH, {8, 16, 32}, {86, 134, 190});
            SDL_Rect listClip{listX + 2, listY + 2, listW - 4, listH - 4};
            SDL_RenderSetClipRect(renderer, &listClip);
            for (int i = 0; i < static_cast<int>(modes.size()); ++i) {
                const int y = listY + 12 + (i - scrollStart) * rowH;
                const bool sel = (g.titleSelection == i);
                const RGB b = sel ? RGB{255, 232, 118} : RGB{94, 150, 214};
                DrawPanel(renderer, listX + 2, y - 10, listW - 4, 54, {10, 18, 36}, b);
                DrawText(renderer, listX + 24, y + 2, 2, sel ? RGB{255, 245, 170} : RGB{190, 220, 255}, modes[i]);
            }
            SDL_RenderSetClipRect(renderer, nullptr);

            if (maxStart > 0) {
                const float t = static_cast<float>(scrollStart) / static_cast<float>(maxStart);
                const int railX = listX + listW - 8;
                SDL_SetRenderDrawColor(renderer, 80, 110, 150, 200);
                SDL_Rect rail{railX, listY + 6, 4, listH - 12};
                SDL_RenderFillRect(renderer, &rail);
                const int thumbH = std::max(20, static_cast<int>((static_cast<float>(visibleRows) / modes.size()) * (listH - 12)));
                const int thumbY = listY + 6 + static_cast<int>(t * ((listH - 12) - thumbH));
                SDL_SetRenderDrawColor(renderer, 180, 220, 255, 220);
                SDL_Rect thumb{railX, thumbY, 4, thumbH};
                SDL_RenderFillRect(renderer, &thumb);
            }

            DrawText(renderer, panelX + 30, panelY + panelH - 66, 2, {140, 255, 190},
                     "ENTER/SPACE TO START  UP/DOWN OR 1-7 TO SELECT");
            DrawText(renderer, panelX + 30, panelY + panelH - 40, 2, {210, 220, 240},
                     "AFTER EACH GAME, NEXT UNPLAYED GAME IS CHOSEN AT RANDOM");
            SDL_RenderPresent(renderer);
            continue;
        }

        if (g.mode == GameMode::LongJumpDuet) {
            g.beatPulse = std::max(0.0f, g.beatPulse - dt * 3.0f);
            g.beatRipple = std::max(0.0f, g.beatRipple - dt * 1.9f);
            g.uiFlash = std::max(0.0f, g.uiFlash - dt * 2.0f);
            g.ljFootGlow1[0] = std::max(0.0f, g.ljFootGlow1[0] - dt * 2.3f);
            g.ljFootGlow1[1] = std::max(0.0f, g.ljFootGlow1[1] - dt * 2.3f);
            g.ljFootGlow2[0] = std::max(0.0f, g.ljFootGlow2[0] - dt * 2.3f);
            g.ljFootGlow2[1] = std::max(0.0f, g.ljFootGlow2[1] - dt * 2.3f);

            const float targetBpm = std::min(136.0f, 116.0f + static_cast<float>(g.longJumpRound - 1) * 4.0f);
            g.longJumpBpm += (targetBpm - g.longJumpBpm) * std::min(1.0f, dt * 2.5f);

            const float stepDur = 60.0f / g.longJumpBpm / 4.0f;
            float secSinceStep = static_cast<float>(now - g.longJumpStepStartTicks) / 1000.0f;
            while (!g.paused && secSinceStep >= stepDur) {
                secSinceStep -= stepDur;
                g.longJumpStepStartTicks += static_cast<uint32_t>(stepDur * 1000.0f);
                g.currentStep = (g.currentStep + 1) % 16;
                const int s = g.currentStep & 15;
                if (s == 0 || s == 4 || s == 8 || s == 12) {
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 95.0f, 0.24f, 12.0f, 0.15f);
                }
                if (s % 2 == 0) {
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.03f, 180.0f, 0.012f);
                }
                if (s == 7 || s == 15) {
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(52 + (g.longJumpRound - 1) * 2), 0.06f, 20.0f, 0.08f);
                }
            }

            const float beatDur = 60.0f / g.longJumpBpm;
            float secSinceBeat = static_cast<float>(now - g.longJumpBeatStartTicks) / 1000.0f;
            while (!g.paused && secSinceBeat >= beatDur) {
                secSinceBeat -= beatDur;
                g.longJumpBeatStartTicks += static_cast<uint32_t>(beatDur * 1000.0f);
                g.beatPulse = 1.0f;
                g.beatRipple = 1.0f;
                g.currentBar++;
                if (g.longJumpBeatsRemaining > 0) g.longJumpBeatsRemaining--;
            }

            const float takeoffLine = 18.0f;
            if (!g.paused) {
                if (!g.ljJumped1 && !g.ljDone1) {
                    g.ljSpeed1 = std::max(2.8f, g.ljSpeed1 - dt * 0.9f);
                    g.ljRun1 += g.ljSpeed1 * dt;
                    if (g.ljRun1 > takeoffLine + 0.35f) {
                        g.ljFoul1 = true;
                        g.ljDone1 = true;
                    }
                }
                if (!g.ljJumped2 && !g.ljDone2) {
                    g.ljSpeed2 = std::max(2.8f, g.ljSpeed2 - dt * 0.9f);
                    g.ljRun2 += g.ljSpeed2 * dt;
                    if (g.ljRun2 > takeoffLine + 0.35f) {
                        g.ljFoul2 = true;
                        g.ljDone2 = true;
                    }
                }

                if (g.ljInAir1) {
                    g.ljFlightX1 += g.ljVx1 * dt;
                    g.ljFlightY1 += g.ljVy1 * dt;
                    g.ljVy1 -= 9.8f * dt * 0.78f;
                    if (g.ljFlightY1 <= 0.0f && g.ljVy1 < 0.0f) {
                        g.ljFlightY1 = 0.0f;
                        g.ljInAir1 = false;
                        g.ljDone1 = true;
                        const float quality = std::clamp(1.0f - std::abs(g.ljRun1 - takeoffLine) / 1.6f, 0.45f, 1.0f);
                        g.ljDist1 = g.ljFoul1 ? 0.0f : std::max(0.0f, g.ljFlightX1 * quality);
                        TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 82.0f, 0.20f, 12.0f, 0.16f);
                        SpawnBurst(g, 410.0f + g.ljDist1 * 15.0f, 540.0f, {255, 230, 140}, 18);
                    }
                }
                if (g.ljInAir2) {
                    g.ljFlightX2 += g.ljVx2 * dt;
                    g.ljFlightY2 += g.ljVy2 * dt;
                    g.ljVy2 -= 9.8f * dt * 0.78f;
                    if (g.ljFlightY2 <= 0.0f && g.ljVy2 < 0.0f) {
                        g.ljFlightY2 = 0.0f;
                        g.ljInAir2 = false;
                        g.ljDone2 = true;
                        const float quality = std::clamp(1.0f - std::abs(g.ljRun2 - takeoffLine) / 1.6f, 0.45f, 1.0f);
                        g.ljDist2 = g.ljFoul2 ? 0.0f : std::max(0.0f, g.ljFlightX2 * quality);
                        TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 86.0f, 0.20f, 12.0f, 0.16f);
                        SpawnBurst(g, 930.0f + g.ljDist2 * 15.0f, 540.0f, {140, 230, 255}, 18);
                    }
                }

                if ((g.ljDone1 || g.ljFoul1) && g.ljDist1 <= 0.0f && g.ljJumped1 && !g.ljInAir1) {
                    g.ljDist1 = 0.0f;
                }
                if ((g.ljDone2 || g.ljFoul2) && g.ljDist2 <= 0.0f && g.ljJumped2 && !g.ljInAir2) {
                    g.ljDist2 = 0.0f;
                }

                if (g.longJumpBeatsRemaining <= 0 && (!g.ljDone1 || !g.ljDone2)) {
                    if (!g.ljDone1) { g.ljDone1 = true; g.ljFoul1 = true; }
                    if (!g.ljDone2) { g.ljDone2 = true; g.ljFoul2 = true; }
                }

                if (g.ljDone1 && g.ljDone2 && g.longJumpResolve <= 0.0f && g.longJumpEndDelay <= 0.0f) {
                    if (g.ljFoul1 && !g.ljFoul2) g.longJumpRoundWinner = 2;
                    else if (g.ljFoul2 && !g.ljFoul1) g.longJumpRoundWinner = 1;
                    else if (g.ljDist1 > g.ljDist2 + 0.01f) g.longJumpRoundWinner = 1;
                    else if (g.ljDist2 > g.ljDist1 + 0.01f) g.longJumpRoundWinner = 2;
                    else g.longJumpRoundWinner = 0;

                    if (g.longJumpRoundWinner == 1) g.longJumpP1Rounds++;
                    else if (g.longJumpRoundWinner == 2) g.longJumpP2Rounds++;
                    g.longJumpResolve = 2.2f;
                    SpawnBurst(g, static_cast<float>(kWindowW / 2), 420.0f, {255, 180, 230}, 30);
                }

                if (g.longJumpResolve > 0.0f) {
                    g.longJumpResolve -= dt;
                    if (g.longJumpResolve <= 0.0f) {
                        const bool matchDone = (g.longJumpP1Rounds >= 2 || g.longJumpP2Rounds >= 2 || g.longJumpRound >= 3);
                        if (matchDone) {
                            g.longJumpEndDelay = 3.0f;
                        } else {
                            g.longJumpRound++;
                            g.longJumpRoundWinner = 0;
                            ResetLongJumpRound(g);
                        }
                    }
                }

                if (g.longJumpEndDelay > 0.0f) {
                    g.longJumpEndDelay -= dt;
                    if (g.longJumpEndDelay <= 0.0f) {
                        if (!AllGameModesPlayed(g)) InitTestCardCooldown(g, RandomUnplayedMode(g));
                        else g.running = false;
                        continue;
                    }
                }
            }

            UpdateParticles(g, dt);

            DrawRetroRoom(renderer, kWindowW, kWindowH);
            DrawPsychedelicWash(renderer, g.songSeconds, 1.0f);
            DrawNeonBeatRings(renderer, g.songSeconds, std::min(1.0f, g.beatPulse + 0.35f), 1.0f);
            for (int y = 0; y < kWindowH; y += 4) {
                const float ph = std::fmod(g.songSeconds * 0.42f + y * 0.003f, 1.0f);
                const RGB c = HsvToRgb(ph, 1.0f, 1.0f);
                SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, static_cast<uint8_t>(20 + 55 * g.beatPulse));
                SDL_Rect s{0, y, kWindowW, 2};
                SDL_RenderFillRect(renderer, &s);
            }
            for (int i = 0; i < 18; ++i) {
                const float ph = std::fmod(g.songSeconds * 0.19f + i * 0.07f, 1.0f);
                const RGB c = HsvToRgb(ph, 1.0f, 1.0f);
                const int w = 120 + i * 56;
                const int h = 70 + i * 34;
                SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, static_cast<uint8_t>(8 + i * 3));
                SDL_Rect r{kWindowW / 2 - w / 2, kWindowH / 2 - h / 2, w, h};
                SDL_RenderDrawRect(renderer, &r);
            }

            const int topX = 90, topY = 46, topW = kWindowW - 180, topH = 118;
            DrawPanel(renderer, topX, topY, topW, topH, {14, 24, 46}, {120, 220, 255});
            DrawText(renderer, topX + 18, topY + 18, 3, {245, 248, 255}, "JEDWARD'S LONGJUMP");
            DrawText(renderer, topX + 18, topY + 54, 2, {255, 232, 118}, "P1: A/D FEET  W/S ANGLE  SPACE JUMP @ LINE");
            DrawText(renderer, topX + 18, topY + 78, 2, {123, 228, 255}, "P2: LEFT/RIGHT FEET  UP/DOWN ANGLE  ENTER JUMP @ LINE");

            const int arenaX = 90, arenaY = 188, arenaW = kWindowW - 180, arenaH = 390;
            DrawPanel(renderer, arenaX, arenaY, arenaW, arenaH, {10, 22, 40}, {96, 170, 255});
            const int laneGap = 24;
            const int laneW = (arenaW - laneGap * 3) / 2;
            const int laneH = arenaH - 80;
            const int p1X = arenaX + laneGap;
            const int p2X = p1X + laneW + laneGap;
            const int laneY = arenaY + 34;
            const float metersToPx = static_cast<float>(laneW - 120) / 28.0f;
            const int takeoffX = p1X + 112 + static_cast<int>(takeoffLine * metersToPx);
            const int takeoffX2 = p2X + 112 + static_cast<int>(takeoffLine * metersToPx);

            auto drawLane = [&](int x, RGB tint) {
                DrawPanel(renderer, x, laneY, laneW, laneH, {8, 18, 32}, tint);
                for (int yy = laneY + 3; yy < laneY + laneH - 3; yy += 6) {
                    const float ph = std::fmod(g.songSeconds * 0.55f + (yy - laneY) * 0.006f + x * 0.0008f, 1.0f);
                    const RGB c = HsvToRgb(ph, 1.0f, 1.0f);
                    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, static_cast<uint8_t>(18 + 48 * g.beatPulse));
                    SDL_RenderDrawLine(renderer, x + 3, yy, x + laneW - 4, yy);
                }
                SDL_SetRenderDrawColor(renderer, 90, 135, 190, 130);
                for (int i = 0; i <= 28; i += 2) {
                    const int gx = x + 112 + static_cast<int>(i * metersToPx);
                    SDL_RenderDrawLine(renderer, gx, laneY + 36, gx, laneY + laneH - 32);
                }
                SDL_SetRenderDrawColor(renderer, 200, 240, 255, 210);
                SDL_RenderDrawLine(renderer, x + 112, laneY + laneH - 32, x + laneW - 24, laneY + laneH - 32);
            };
            drawLane(p1X, {255, 170, 120});
            drawLane(p2X, {120, 220, 255});

            SDL_SetRenderDrawColor(renderer, 255, 240, 130, 240);
            SDL_RenderDrawLine(renderer, takeoffX, laneY + 26, takeoffX, laneY + laneH - 24);
            SDL_RenderDrawLine(renderer, takeoffX2, laneY + 26, takeoffX2, laneY + laneH - 24);
            DrawText(renderer, takeoffX - 20, laneY + 8, 1, {255, 240, 130}, "LINE");
            DrawText(renderer, takeoffX2 - 20, laneY + 8, 1, {255, 240, 130}, "LINE");

            const int runnerY1 = laneY + laneH - 54 - static_cast<int>(g.ljFlightY1 * 22.0f);
            const int runnerY2 = laneY + laneH - 54 - static_cast<int>(g.ljFlightY2 * 22.0f);
            const float meterX1 = g.ljJumped1 ? (takeoffLine + g.ljFlightX1) : g.ljRun1;
            const float meterX2 = g.ljJumped2 ? (takeoffLine + g.ljFlightX2) : g.ljRun2;
            const int runnerLaneOffset1 = (g.ljLastFoot1 == 0) ? -9 : 9;
            const int runnerLaneOffset2 = (g.ljLastFoot2 == 0) ? -9 : 9;
            const int runnerX1 = p1X + 112 + static_cast<int>(meterX1 * metersToPx) + runnerLaneOffset1;
            const int runnerX2 = p2X + 112 + static_cast<int>(meterX2 * metersToPx) + runnerLaneOffset2;

            for (int i = 0; i < 4; ++i) {
                const float ph = std::fmod(g.songSeconds * 0.34f + i * 0.11f, 1.0f);
                const RGB c = HsvToRgb(ph, 1.0f, 1.0f);
                SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, static_cast<uint8_t>(35 + 45 * g.beatPulse));
                SDL_Rect a1{runnerX1 - 24 - i * 10, runnerY1 - 30 - i * 3, 32 + i * 6, 30 + i * 6};
                SDL_Rect a2{runnerX2 - 24 - i * 10, runnerY2 - 30 - i * 3, 32 + i * 6, 30 + i * 6};
                SDL_RenderDrawRect(renderer, &a1);
                SDL_RenderDrawRect(renderer, &a2);
            }

            SDL_SetRenderDrawColor(renderer, 255, 232, 118, 230);
            SDL_Rect r1{runnerX1 - 10, runnerY1 - 24, 20, 24};
            SDL_RenderFillRect(renderer, &r1);
            SDL_SetRenderDrawColor(renderer, 245, 248, 255, 220);
            SDL_RenderDrawRect(renderer, &r1);
            DrawText(renderer, runnerX1 - 6, runnerY1 - 34, 1, {245, 248, 255}, "1");

            SDL_SetRenderDrawColor(renderer, 130, 230, 255, 230);
            SDL_Rect r2{runnerX2 - 10, runnerY2 - 24, 20, 24};
            SDL_RenderFillRect(renderer, &r2);
            SDL_SetRenderDrawColor(renderer, 245, 248, 255, 220);
            SDL_RenderDrawRect(renderer, &r2);
            DrawText(renderer, runnerX2 - 6, runnerY2 - 34, 1, {245, 248, 255}, "2");

            const int beatMeterW = laneW - 148;
            const int beatX1 = p1X + 106;
            const int beatX2 = p2X + 106;
            const int beatY = laneY + 10;
            const float beatPhase = std::clamp(secSinceBeat / std::max(0.0001f, beatDur), 0.0f, 1.0f);
            SDL_SetRenderDrawColor(renderer, 28, 30, 42, 230);
            SDL_Rect bm1{beatX1, beatY, beatMeterW, 14};
            SDL_Rect bm2{beatX2, beatY, beatMeterW, 14};
            SDL_RenderFillRect(renderer, &bm1);
            SDL_RenderFillRect(renderer, &bm2);
            SDL_SetRenderDrawColor(renderer, 210, 220, 235, 180);
            SDL_RenderDrawRect(renderer, &bm1);
            SDL_RenderDrawRect(renderer, &bm2);
            const int phx1 = beatX1 + static_cast<int>(beatPhase * (beatMeterW - 1));
            const int phx2 = beatX2 + static_cast<int>(beatPhase * (beatMeterW - 1));
            SDL_SetRenderDrawColor(renderer, 120, 255, 170, 220);
            SDL_RenderDrawLine(renderer, phx1, beatY - 2, phx1, beatY + 16);
            SDL_SetRenderDrawColor(renderer, 120, 220, 255, 220);
            SDL_RenderDrawLine(renderer, phx2, beatY - 2, phx2, beatY + 16);

            auto drawFootLanes = [&](int x, const std::array<float, 2>& glow, int expected, RGB baseA, RGB baseB) {
                const int fx = x + 18;
                const int fy = laneY + 22;
                const int fw = 34;
                const int fh = laneH - 54;
                const int gap = 8;
                const int hitY = fy + fh - 42;
                const float beatBlink = 0.55f + 0.45f * std::sin(beatPhase * kTau);
                for (int foot = 0; foot < 2; ++foot) {
                    const int lx = fx + foot * (fw + gap);
                    const RGB base = (foot == 0) ? baseA : baseB;
                    SDL_SetRenderDrawColor(renderer, 20, 26, 40, 220);
                    SDL_Rect bg{lx, fy, fw, fh};
                    SDL_RenderFillRect(renderer, &bg);
                    SDL_SetRenderDrawColor(renderer, base.r, base.g, base.b, 180);
                    SDL_RenderDrawRect(renderer, &bg);

                    SDL_SetRenderDrawColor(renderer, 120, 255, 170, (foot == expected) ? 150 : 70);
                    SDL_Rect hit{lx + 2, hitY, fw - 4, 16};
                    SDL_RenderFillRect(renderer, &hit);
                    if (foot == expected) {
                        SDL_SetRenderDrawColor(renderer, 255, 248, 180, static_cast<uint8_t>(140 + 90 * beatBlink));
                        SDL_Rect activeFrame{lx - 3, fy - 3, fw + 6, fh + 6};
                        SDL_RenderDrawRect(renderer, &activeFrame);
                    }

                    const float glowAmt = std::clamp(glow[foot], 0.0f, 1.0f);
                    if (glowAmt > 0.01f) {
                        SDL_SetRenderDrawColor(renderer, 255, 250, 180, static_cast<uint8_t>(40 + 180 * glowAmt));
                        SDL_Rect pulse{lx - 2, fy + 2, fw + 4, fh - 4};
                        SDL_RenderFillRect(renderer, &pulse);
                    }
                    DrawText(renderer, lx + 12, fy - 14, 1, {245, 248, 255}, (foot == 0) ? "L" : "R");
                }

                const int sweepY = fy + static_cast<int>(beatPhase * static_cast<float>(fh - 1));
                SDL_SetRenderDrawColor(renderer, 245, 248, 255, 210);
                SDL_RenderDrawLine(renderer, fx - 2, sweepY, fx + 2 * fw + gap + 2, sweepY);

                const std::string nowFoot = (expected == 0) ? "NOW LEFT" : "NOW RIGHT";
                DrawText(renderer, fx - 2, fy - 30, 2, {255, 248, 180}, nowFoot);
            };
            drawFootLanes(p1X, g.ljFootGlow1, g.ljExpectedFoot1, {255, 190, 130}, {255, 232, 118});
            drawFootLanes(p2X, g.ljFootGlow2, g.ljExpectedFoot2, {120, 210, 255}, {160, 245, 255});

            DrawText(renderer, p1X + 24, laneY + laneH - 20, 2, {255, 232, 118},
                     std::string("NEXT:") + (g.ljExpectedFoot1 == 0 ? "L" : "R") +
                         " SPD:" + std::to_string(static_cast<int>(std::round(g.ljSpeed1))) +
                         " ANG:" + std::to_string(static_cast<int>(std::round(g.ljAngle1))));
            DrawText(renderer, p2X + 24, laneY + laneH - 20, 2, {123, 228, 255},
                     std::string("NEXT:") + (g.ljExpectedFoot2 == 0 ? "L" : "R") +
                         " SPD:" + std::to_string(static_cast<int>(std::round(g.ljSpeed2))) +
                         " ANG:" + std::to_string(static_cast<int>(std::round(g.ljAngle2))));

            DrawParticles(renderer, g);

            const int hudY = 600;
            DrawPanel(renderer, 90, hudY, kWindowW - 180, 146, {12, 24, 48}, {96, 170, 255});
            DrawText(renderer, 120, hudY + 18, 2, {255, 232, 118},
                     "ROUND " + std::to_string(g.longJumpRound) + "/3  P1 ROUNDS:" + std::to_string(g.longJumpP1Rounds) +
                         "  P2 ROUNDS:" + std::to_string(g.longJumpP2Rounds));
            DrawText(renderer, 120, hudY + 44, 2, {123, 228, 255},
                     "P1 DIST:" + std::to_string(static_cast<int>(std::round(g.ljDist1 * 10.0f))) +
                         "cm x10  " + (g.ljFoul1 ? std::string("FOUL") : std::string("VALID")) +
                         "  |  P2 DIST:" + std::to_string(static_cast<int>(std::round(g.ljDist2 * 10.0f))) +
                         "cm x10  " + (g.ljFoul2 ? std::string("FOUL") : std::string("VALID")));
            DrawText(renderer, 120, hudY + 72, 2, {238, 244, 255},
                     "TIME:" + std::to_string(static_cast<int>(std::ceil(std::max(0.0f, g.longJumpBeatsRemaining * (60.0f / g.longJumpBpm))))) +
                         "  ALTERNATE FEET ON BEAT, SET ANGLE, JUMP AT THE LINE");

            std::string status = "STATUS: RUN";
            if (g.longJumpRoundWinner == 1) status = "STATUS: ROUND TO P1";
            else if (g.longJumpRoundWinner == 2) status = "STATUS: ROUND TO P2";
            else if (g.longJumpResolve > 0.0f) status = "STATUS: DRAW ROUND";
            if (g.longJumpEndDelay > 0.0f) {
                if (g.longJumpP1Rounds > g.longJumpP2Rounds) status = "STATUS: MATCH WINNER P1";
                else if (g.longJumpP2Rounds > g.longJumpP1Rounds) status = "STATUS: MATCH WINNER P2";
                else status = "STATUS: MATCH DRAW";
            }
            DrawText(renderer, 120, hudY + 100, 2,
                     (g.longJumpRoundWinner == 1) ? RGB{255, 232, 118} :
                     (g.longJumpRoundWinner == 2) ? RGB{123, 228, 255} : RGB{140, 255, 170},
                     status);

            SDL_RenderPresent(renderer);
            continue;
        }

        if (g.mode == GameMode::SnakeDuet) {
            g.beatPulse = std::max(0.0f, g.beatPulse - dt * 3.1f);
            g.beatRipple = std::max(0.0f, g.beatRipple - dt * 2.0f);
            g.uiFlash = std::max(0.0f, g.uiFlash - dt * 1.9f);

            const float targetBpm = std::min(148.0f, 110.0f + static_cast<float>(g.snakeCombo) * 0.8f);
            g.snakeBpm += (targetBpm - g.snakeBpm) * std::min(1.0f, dt * 2.0f);

            const float stepDur = 60.0f / g.snakeBpm / 2.0f;  // 8th-note movement
            float secSinceStep = static_cast<float>(now - g.snakeStepStartTicks) / 1000.0f;
            while (!g.paused && secSinceStep >= stepDur) {
                secSinceStep -= stepDur;
                g.snakeStepStartTicks += static_cast<uint32_t>(stepDur * 1000.0f);
                g.currentStep = (g.currentStep + 1) % 16;

                if (!g.snakeGameOver) {
                    g.snakeDir1 = g.snakePendingDir1;
                    g.snakeDir2 = g.snakePendingDir2;

                    auto nextHead = [](SDL_Point p, int dir) {
                        if (dir == 0) p.y -= 1;
                        else if (dir == 1) p.x += 1;
                        else if (dir == 2) p.y += 1;
                        else p.x -= 1;
                        return p;
                    };
                    SDL_Point n1 = nextHead(g.snake1.front(), g.snakeDir1);
                    SDL_Point n2 = nextHead(g.snake2.front(), g.snakeDir2);
                    const bool grow1 = (n1.x == g.snakeFood.x && n1.y == g.snakeFood.y);
                    const bool grow2 = (n2.x == g.snakeFood.x && n2.y == g.snakeFood.y);

                    auto contains = [](const std::vector<SDL_Point>& b, SDL_Point p, bool ignoreTail) {
                        const size_t lim = (ignoreTail && !b.empty()) ? b.size() - 1 : b.size();
                        for (size_t i = 0; i < lim; ++i) if (b[i].x == p.x && b[i].y == p.y) return true;
                        return false;
                    };
                    auto outBounds = [&](SDL_Point p) {
                        return p.x < 0 || p.y < 0 || p.x >= g.snakeGridW || p.y >= g.snakeGridH;
                    };

                    bool dead1 = outBounds(n1) || contains(g.snake1, n1, !grow1) || contains(g.snake2, n1, !grow2);
                    bool dead2 = outBounds(n2) || contains(g.snake2, n2, !grow2) || contains(g.snake1, n2, !grow1);
                    if (n1.x == n2.x && n1.y == n2.y) { dead1 = true; dead2 = true; }

                    if (dead1 || dead2) {
                        g.snakeGameOver = true;
                        g.snakeEndDelay = std::max(g.snakeEndDelay, 2.8f);
                        g.snakeCombo = 0;
                        g.uiFlash = 1.0f;
                        TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.22f, 25.0f, 0.10f);
                        SpawnBurst(g, 680.0f, 420.0f, {255, 120, 120}, 36);
                    } else {
                        g.snake1.insert(g.snake1.begin(), n1);
                        g.snake2.insert(g.snake2.begin(), n2);
                        if (!grow1) g.snake1.pop_back();
                        if (!grow2) g.snake2.pop_back();

                        if (grow1) {
                            g.snakeP1Score += 10;
                            g.snakeScore += 10;
                            g.snakeCombo++;
                            g.beatPulse = 1.0f;
                            TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(72), 0.08f, 24.0f, 0.08f);
                            SpawnBurst(g, 180.0f + n1.x * 40.0f, 230.0f + n1.y * 28.0f, {255, 210, 120}, 14);
                            SpawnSnakeFood(g);
                        }
                        if (grow2) {
                            g.snakeP2Score += 10;
                            g.snakeScore += 10;
                            g.snakeCombo++;
                            g.beatPulse = 1.0f;
                            TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(76), 0.08f, 24.0f, 0.08f);
                            SpawnBurst(g, 180.0f + n2.x * 40.0f, 230.0f + n2.y * 28.0f, {120, 220, 255}, 14);
                            SpawnSnakeFood(g);
                        }
                    }
                }

                // Musical snake engine: kick-led with minor notes from head positions.
                const int s = g.currentStep & 15;
                const float funk = std::clamp(0.45f + 0.08f * g.snakeCombo, 0.0f, 1.0f);
                static const std::array<int, 16> kickPatA = {{1,0,0,1, 0,1,0,0, 1,0,1,0, 0,1,0,1}};
                static const std::array<int, 16> kickPatB = {{1,0,1,0, 0,1,0,1, 1,0,0,1, 0,1,1,0}};
                static const std::array<int, 16> hatPat =   {{0,1,1,0, 1,0,1,1, 0,1,1,0, 1,1,0,1}};
                const bool alt = ((g.currentBar / 2) % 2) == 1;
                if ((alt ? kickPatB[s] : kickPatA[s]) != 0) {
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 92.0f, 0.28f + 0.12f * funk, 10.5f, 0.16f);
                    if (s == 6 || s == 14) TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 74.0f, 0.10f + 0.06f * funk, 16.0f, 0.10f);
                }
                if (hatPat[s]) {
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.03f + 0.03f * funk, 170.0f, 0.011f);
                }
                if (s == 4 || s == 12 || s == 11) {
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.07f + 0.03f * funk, 44.0f, 0.03f);
                }
                static const std::array<int, 8> minor = {{0, 2, 3, 5, 7, 8, 10, 12}};
                const int m1 = 40 + minor[(g.snakeGridH - 1 - g.snake1.front().y) % 8];
                const int m2 = 47 + minor[(g.snakeGridH - 1 - g.snake2.front().y) % 8];
                if ((s % 2) == 0 || s == 3 || s == 11) {
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(m1 + 12), 0.10f + 0.03f * funk, 19.0f, 0.08f);
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(m2 + 12), 0.08f + 0.03f * funk, 19.0f, 0.08f);
                }
                if (s == 7 || s == 15) {
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(m1 + 24), 0.05f + 0.03f * funk, 24.0f, 0.05f);
                }
            }

            const float beatDur = 60.0f / g.snakeBpm;
            float secSinceBeat = static_cast<float>(now - g.snakeBeatStartTicks) / 1000.0f;
            while (!g.paused && secSinceBeat >= beatDur) {
                secSinceBeat -= beatDur;
                g.snakeBeatStartTicks += static_cast<uint32_t>(beatDur * 1000.0f);
                g.currentBar++;
                g.beatPulse = 1.0f;
                g.beatRipple = 1.0f;
                if (g.snakeBeatsRemaining > 0) g.snakeBeatsRemaining--;
                SpawnBurst(g, static_cast<float>(kWindowW / 2), static_cast<float>(kRailArenaY + 180), HsvToRgb(std::fmod(g.songSeconds * 0.11f, 1.0f), 1.0f, 1.0f), 18);
            }
            const float beatPhase = std::clamp(secSinceBeat / std::max(0.0001f, beatDur), 0.0f, 1.0f);

            UpdateParticles(g, dt);

            if (!g.paused && g.snakeBeatsRemaining <= 0 && g.snakeEndDelay <= 0.0f) {
                g.snakeEndDelay = 2.6f;
            }
            if (!g.paused && g.snakeEndDelay > 0.0f) {
                g.snakeEndDelay -= dt;
                if (g.snakeEndDelay <= 0.0f) {
                    if (!AllGameModesPlayed(g)) InitTestCardCooldown(g, RandomUnplayedMode(g));
                    else g.running = false;
                    continue;
                }
            }

            DrawRetroRoom(renderer, kWindowW, kWindowH);
            DrawPsychedelicWash(renderer, g.songSeconds, std::clamp(0.55f + 0.30f * g.beatPulse + 0.02f * g.snakeCombo, 0.0f, 1.0f));
            DrawNeonBeatRings(renderer, g.songSeconds, g.beatPulse, 0.65f);
            for (int y = 0; y < kWindowH; y += 9) {
                const float w = 0.5f + 0.5f * std::sin(g.songSeconds * 8.0f + y * 0.04f);
                const RGB c = HsvToRgb(std::fmod(g.songSeconds * 0.16f + y * 0.0015f, 1.0f), 1.0f, 1.0f);
                SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, static_cast<uint8_t>(8 + 22 * w + 24 * g.beatPulse));
                SDL_Rect band{0, y, kWindowW, 4};
                SDL_RenderFillRect(renderer, &band);
            }

            const int panelX = 80, panelY = 50, panelW = kWindowW - 160, panelH = 120;
            DrawPanel(renderer, panelX, panelY, panelW, panelH, {12, 24, 52}, {120, 214, 255});
            DrawText(renderer, panelX + 18, panelY + 20, 3, {245, 248, 255}, "DOUBLE SNAKE");
            DrawText(renderer, panelX + 18, panelY + 58, 2, {255, 232, 118}, "P1 SNAKE: W/A/S/D");
            DrawText(renderer, panelX + 18, panelY + 82, 2, {123, 228, 255}, "P2 SNAKE: ARROWS  SHARED FOOD, SHARED GRID");

            const int arenaX = 120, arenaY = 210, arenaW = kWindowW - 240, arenaH = 360;
            DrawPanel(renderer, arenaX, arenaY, arenaW, arenaH, {10, 20, 44}, {110, 190, 255});

            const int cellW = (arenaW - 140) / g.snakeGridW;
            const int cellH = (arenaH - 110) / g.snakeGridH;
            const int gridW = cellW * g.snakeGridW;
            const int gridH = cellH * g.snakeGridH;
            const int gridX = arenaX + (arenaW - gridW) / 2;
            const int gridY = arenaY + 40;

            // OTT color fog behind the board.
            for (int i = 0; i < 12; ++i) {
                const float t = std::fmod(g.songSeconds * 0.28f + i * 0.09f, 1.0f);
                const RGB c = HsvToRgb(t, 1.0f, 1.0f);
                SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, static_cast<uint8_t>(8 + 12 * g.beatPulse));
                SDL_Rect glow{gridX - 18 - i * 2, gridY - 12 - i, gridW + 36 + i * 4, gridH + 24 + i * 2};
                SDL_RenderDrawRect(renderer, &glow);
            }

            for (int x = 0; x <= g.snakeGridW; ++x) {
                const int px = gridX + x * cellW;
                SDL_SetRenderDrawColor(renderer, 90, 130, 190, (x % 2 == 0) ? 120 : 70);
                SDL_RenderDrawLine(renderer, px, gridY, px, gridY + gridH);
            }
            for (int y = 0; y <= g.snakeGridH; ++y) {
                const int py = gridY + y * cellH;
                SDL_SetRenderDrawColor(renderer, 90, 130, 190, (y % 2 == 0) ? 120 : 70);
                SDL_RenderDrawLine(renderer, gridX, py, gridX + gridW, py);
            }

            const float playX = static_cast<float>(gridX) + beatPhase * static_cast<float>(gridW);
            for (int i = -2; i <= 2; ++i) {
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, static_cast<uint8_t>(40 + (2 - std::abs(i)) * 70 + 60 * g.beatPulse));
                SDL_RenderDrawLine(renderer, static_cast<int>(playX) + i, gridY - 10, static_cast<int>(playX) + i, gridY + gridH + 10);
            }

            // Food
            {
                const int fx = gridX + g.snakeFood.x * cellW;
                const int fy = gridY + g.snakeFood.y * cellH;
                const float p = 0.6f + 0.4f * std::sin(g.songSeconds * 10.0f);
                SDL_SetRenderDrawColor(renderer, 255, 220, 120, static_cast<uint8_t>(120 + 100 * p));
                SDL_Rect f{fx + 2, fy + 2, std::max(3, cellW - 4), std::max(3, cellH - 4)};
                SDL_RenderFillRect(renderer, &f);
                SDL_SetRenderDrawColor(renderer, 255, 245, 190, 240);
                SDL_RenderDrawRect(renderer, &f);
            }

            auto drawSnake = [&](const std::vector<SDL_Point>& body, RGB bodyCol, RGB headCol, const char* tag) {
                for (size_t i = 0; i < body.size(); ++i) {
                    const int x = gridX + body[i].x * cellW;
                    const int y = gridY + body[i].y * cellH;
                    const bool head = (i == 0);
                    const RGB c = head ? headCol : bodyCol;
                    const RGB aura = HsvToRgb(std::fmod(g.songSeconds * 0.22f + static_cast<float>(i) * 0.03f, 1.0f), 1.0f, 1.0f);
                    SDL_SetRenderDrawColor(renderer, aura.r, aura.g, aura.b, static_cast<uint8_t>(20 + 45 * g.beatPulse));
                    SDL_Rect halo{x - 2, y - 2, std::max(5, cellW), std::max(5, cellH)};
                    SDL_RenderFillRect(renderer, &halo);
                    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 220);
                    SDL_Rect seg{x + 2, y + 2, std::max(3, cellW - 4), std::max(3, cellH - 4)};
                    SDL_RenderFillRect(renderer, &seg);
                    SDL_SetRenderDrawColor(renderer, 240, 248, 255, head ? 240 : 120);
                    SDL_RenderDrawRect(renderer, &seg);
                    if (i > 0) {
                        const int px = gridX + body[i - 1].x * cellW + cellW / 2;
                        const int py = gridY + body[i - 1].y * cellH + cellH / 2;
                        const int cx = x + cellW / 2;
                        const int cy = y + cellH / 2;
                        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 120);
                        SDL_RenderDrawLine(renderer, px, py, cx, cy);
                    }
                    if (head) DrawText(renderer, x + 2, y + 2, 1, {245, 248, 255}, tag);
                }
            };
            drawSnake(g.snake1, {255, 180, 90}, {255, 232, 118}, "1");
            drawSnake(g.snake2, {90, 210, 255}, {140, 245, 255}, "2");

            DrawParticles(renderer, g);

            const int hudY = 600;
            DrawPanel(renderer, 120, hudY, kWindowW - 240, 140, {12, 24, 52}, {96, 170, 255});
            DrawText(renderer, 148, hudY + 18, 2, {255, 232, 118},
                     "SCORE:" + std::to_string(g.snakeScore) + "  COMBO:" + std::to_string(g.snakeCombo) +
                     "  P1:" + std::to_string(g.snakeP1Score) + "  P2:" + std::to_string(g.snakeP2Score));
            DrawText(renderer, 148, hudY + 44, 2, {123, 228, 255},
                     "BPM:" + std::to_string(static_cast<int>(std::round(g.snakeBpm))) +
                     "  LENGTHS:" + std::to_string(static_cast<int>(g.snake1.size())) + "/" + std::to_string(static_cast<int>(g.snake2.size())));
            DrawText(renderer, 148, hudY + 72, 2, {238, 244, 255},
                     "TIME:" + std::to_string(static_cast<int>(std::ceil(std::max(0.0f, g.snakeBeatsRemaining * (60.0f / g.snakeBpm))))) +
                     "  EAT FOOD, AVOID COLLISIONS");
            if (!g.snakeGameOver) {
                DrawText(renderer, 148, hudY + 98, 2, {140, 255, 170}, "STATUS: LIVE");
            } else {
                DrawText(renderer, 148, hudY + 98, 2, {255, 140, 140}, "STATUS: CRASH");
            }

            if (g.snakeEndDelay > 0.0f) {
                const int finalScore = g.snakeScore + g.snakeCombo * 8 - (g.snakeGameOver ? 80 : 0);
                DrawPanel(renderer, 420, hudY + 106, kWindowW - 840, 28, {24, 18, 32}, {255, 200, 120});
                DrawText(renderer, 440, hudY + 112, 2, {255, 232, 118}, "RESULT " + std::to_string(finalScore));
            }

            SDL_RenderPresent(renderer);
            continue;
        }

        if (g.mode == GameMode::NuclearRhythmWar) {
            g.beatPulse = std::max(0.0f, g.beatPulse - dt * 3.2f);
            g.beatRipple = std::max(0.0f, g.beatRipple - dt * 2.1f);
            g.uiFlash = std::max(0.0f, g.uiFlash - dt * 1.8f);
            g.nuclearFlash = std::max(0.0f, g.nuclearFlash - dt * 2.6f);

            const float targetBpm = std::min(149.0f, 93.6f + static_cast<float>(g.nuclearEscalation) * 0.306f);
            g.nuclearBpm += (targetBpm - g.nuclearBpm) * std::min(1.0f, dt * 3.0f);

            const float stepDur = 60.0f / g.nuclearBpm / 4.0f;
            float secSinceStep = static_cast<float>(now - g.nuclearStepStartTicks) / 1000.0f;
            while (!g.paused && secSinceStep >= stepDur) {
                secSinceStep -= stepDur;
                g.nuclearStepStartTicks += static_cast<uint32_t>(stepDur * 1000.0f);
                g.currentStep = (g.currentStep + 1) % 16;
                const int s = g.currentStep & 15;
                const float intensity = std::clamp(g.nuclearEscalation / 100.0f, 0.0f, 1.0f);

                // Skittering IDM-inspired drum lattice.
                static const std::array<int, 16> kickPat = {{1,0,1,1, 0,1,1,0, 1,0,1,1, 0,1,1,0}};
                static const std::array<int, 16> hatPatA = {{1,0,1,1, 0,1,0,1, 1,1,0,1, 0,1,1,0}};
                static const std::array<int, 16> hatPatB = {{0,1,1,0, 1,0,1,1, 0,1,1,0, 1,1,0,1}};
                const bool useB = ((g.currentBar / 2) % 2) == 1;
                if (kickPat[s]) {
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 92.0f, 0.30f + 0.12f * intensity, 11.0f, 0.17f);
                    if ((s % 4) == 0 || s == 6 || s == 14) {
                        TriggerVoice(audio, JuceAudioEngine::VoiceType::Kick, 78.0f, 0.13f + 0.07f * intensity, 15.0f, 0.10f);
                    }
                }
                if ((useB ? hatPatB[s] : hatPatA[s]) != 0) {
                    const float hatGain = ((s % 4) == 3 ? 0.06f : 0.035f) + 0.02f * intensity;
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, hatGain, 140.0f, 0.013f);
                }
                if ((s % 2) == 1 && (s == 1 || s == 5 || s == 7 || s == 11 || s == 13 || s == 15)) {
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.022f + 0.014f * intensity, 220.0f, 0.010f);
                }

                // Minor-key melodic engine.
                static const std::array<int, 16> minorLeadA = {{0, 3, 7, 10, 7, 5, 3, 2, 0, 3, 8, 10, 12, 10, 8, 7}};
                static const std::array<int, 16> minorLeadB = {{0, 2, 5, 8, 5, 3, 2, 0, -2, 0, 3, 7, 8, 7, 5, 3}};
                const int root = 40 + ((g.currentBar / 8) % 2) * 2;
                const int semi = (useB ? minorLeadB[s] : minorLeadA[s]);
                if ((s % 2) == 0 || s == 3 || s == 11) {
                    const int midi = root + semi + 12;
                    const float gain = ((s % 4) == 0 ? 0.12f : 0.085f) + 0.05f * intensity;
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(midi), gain, 22.0f - 6.0f * intensity, 0.09f);
                }
                if (s == 6 || s == 14) {
                    const int midi = root + semi + 24;
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(midi), 0.05f + 0.03f * intensity, 28.0f, 0.05f);
                }
                if ((s % 4) == 0) {
                    const int bassMidi = root + ((useB ? minorLeadB[s] : minorLeadA[s]) % 12);
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(bassMidi), 0.08f + 0.04f * intensity, 18.0f, 0.12f);
                }
                if (s == 7 || s == 15) {
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(root + 31), 0.03f + 0.02f * intensity, 30.0f, 0.035f);
                }
            }

            const float beatDur = 60.0f / g.nuclearBpm;
            float secSinceBeat = static_cast<float>(now - g.nuclearBeatStartTicks) / 1000.0f;
            while (!g.paused && secSinceBeat >= beatDur) {
                secSinceBeat -= beatDur;
                g.nuclearBeatStartTicks += static_cast<uint32_t>(beatDur * 1000.0f);
                g.beatPulse = 1.0f;
                g.beatRipple = 1.0f;
                g.currentBar++;
                if (g.nuclearBeatsRemaining > 0) g.nuclearBeatsRemaining--;
                SpawnBurst(g, static_cast<float>(kWindowW / 2), static_cast<float>(kRailArenaY + 170), {255, 130, 170}, 14);

                const int doctrineIdx = g.currentBar % 8;
                const int doctrine = g.nuclearDoctrine[doctrineIdx];

                std::array<int, 8> nextThreat{{0, 0, 0, 0, 0, 0, 0, 0}};
                const int threatCount = std::clamp(1 + g.nuclearEscalation / 35 + doctrine / 3, 1, 4);
                std::uniform_int_distribution<int> laneDist(0, 7);
                for (int i = 0; i < threatCount; ++i) {
                    const int lane = laneDist(g.visualRng);
                    nextThreat[lane] = std::max(nextThreat[lane], 1 + (doctrine >= 2 ? 1 : 0) + (g.nuclearEscalation >= 70 ? 1 : 0));
                }
                g.nuclearThreat = nextThreat;

                int missedThisBeat = 0;
                int savedThisBeat = 0;
                for (int lane = 0; lane < 8; ++lane) {
                    if (g.nuclearThreat[lane] <= 0) continue;
                    if (g.nuclearIntercept[lane]) {
                        savedThisBeat += g.nuclearThreat[lane];
                        g.nuclearSaved += g.nuclearThreat[lane];
                        g.nuclearCombo++;
                        SpawnBurst(g, static_cast<float>(180 + lane * 120), 430.0f, {120, 235, 255}, 12);
                    } else {
                        missedThisBeat += g.nuclearThreat[lane];
                        g.nuclearMissed += g.nuclearThreat[lane];
                        g.nuclearDevastation = std::min(100, g.nuclearDevastation + g.nuclearThreat[lane] * (3 + doctrine));
                        g.nuclearStability = std::max(0, g.nuclearStability - g.nuclearThreat[lane] * 2);
                        g.nuclearCombo = 0;
                        g.nuclearFlash = 1.0f;
                        SpawnBurst(g, static_cast<float>(180 + lane * 120), 430.0f, {255, 120, 120}, 16);
                        TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.22f, 30.0f, 0.08f);
                    }
                }
                if (savedThisBeat > 0) {
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(79), 0.05f + savedThisBeat * 0.01f, 22.0f, 0.06f);
                    g.uiFlash = std::min(1.0f, g.uiFlash + 0.35f);
                }
                if (missedThisBeat > 0) {
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Noise, 0.0f, 0.18f + missedThisBeat * 0.02f, 24.0f, 0.06f);
                    g.nuclearFlash = 1.0f;
                }
                g.nuclearIntercept = {{false, false, false, false, false, false, false, false}};

                const int escalationDelta = static_cast<int>(std::lround((doctrine * 2 + missedThisBeat * 2 - savedThisBeat) * 0.8f));
                g.nuclearEscalation = std::clamp(g.nuclearEscalation + escalationDelta, 0, 100);
                g.nuclearPsy = std::clamp(0.45f + g.nuclearEscalation * 0.006f + g.nuclearFlash * 0.4f, 0.0f, 1.0f);

                if (g.nuclearDevastation >= 100) {
                    g.nuclearBeatsRemaining = 0;
                    g.nuclearEndDelay = std::max(g.nuclearEndDelay, 2.8f);
                }
            }
            const float beatPhase = std::clamp(secSinceBeat / std::max(0.0001f, beatDur), 0.0f, 1.0f);

            UpdateParticles(g, dt);

            if (!g.paused && g.nuclearBeatsRemaining <= 0 && g.nuclearEndDelay <= 0.0f) {
                g.nuclearEndDelay = 2.8f;
            }
            if (!g.paused && g.nuclearEndDelay > 0.0f) {
                g.nuclearEndDelay -= dt;
                if (g.nuclearEndDelay <= 0.0f) {
                    if (!AllGameModesPlayed(g)) InitTestCardCooldown(g, RandomUnplayedMode(g));
                    else g.running = false;
                    continue;
                }
            }

            DrawRetroRoom(renderer, kWindowW, kWindowH);
            DrawPsychedelicWash(renderer, g.songSeconds, std::clamp(0.55f + g.nuclearPsy * 0.5f, 0.0f, 1.0f));
            DrawNeonBeatRings(renderer, g.songSeconds, g.beatPulse, 0.70f);
            // Saturated war-room strobe.
            for (int y = 0; y < kWindowH; y += 10) {
                const float w = 0.5f + 0.5f * std::sin(g.songSeconds * 9.0f + y * 0.06f);
                SDL_SetRenderDrawColor(renderer, 255, 80, 130, static_cast<uint8_t>(6 + 20 * w + 24 * g.beatPulse));
                SDL_Rect band{0, y, kWindowW, 5};
                SDL_RenderFillRect(renderer, &band);
            }

            const int panelX = 80, panelY = 50, panelW = kWindowW - 160, panelH = 120;
            DrawPanel(renderer, panelX, panelY, panelW, panelH, {28, 12, 18}, {255, 120, 120});
            DrawText(renderer, panelX + 18, panelY + 20, 3, {245, 248, 255}, "STRANGELOVE");
            DrawText(renderer, panelX + 18, panelY + 58, 2, {255, 232, 118}, "P1 COMMAND: A/D WINDOW  W/S DOCTRINE  BACKSPACE HOLD ALL");
            DrawText(renderer, panelX + 18, panelY + 82, 2, {123, 228, 255}, "P2 DEFENSE: LEFT/RIGHT TARGET  ENTER INTERCEPT ON BEAT");

            const int arenaX = 120, arenaY = 210, arenaW = kWindowW - 240, arenaH = 360;
            DrawPanel(renderer, arenaX, arenaY, arenaW, arenaH, {20, 8, 22}, {255, 120, 140});

            const int doctrineX = arenaX + 60;
            const int doctrineY = arenaY + 40;
            const int doctrineW = arenaW - 120;
            const int doctrineCellW = doctrineW / 8;
            for (int i = 0; i < 8; ++i) {
                const int x = doctrineX + i * doctrineCellW;
                SDL_SetRenderDrawColor(renderer, 42, 22, 32, 220);
                SDL_Rect bg{x + 2, doctrineY + 2, doctrineCellW - 4, 40};
                SDL_RenderFillRect(renderer, &bg);
                const int level = std::clamp(g.nuclearDoctrine[i], 0, 3);
                const RGB c = (level == 0) ? RGB{120, 170, 200} : (level == 1) ? RGB{220, 220, 120} : (level == 2) ? RGB{255, 180, 90} : RGB{255, 110, 110};
                SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 230);
                SDL_Rect fill{x + 4, doctrineY + 40 - level * 10, doctrineCellW - 8, 6 + level * 10};
                SDL_RenderFillRect(renderer, &fill);
                SDL_SetRenderDrawColor(renderer, 255, 240, 220, static_cast<uint8_t>(50 + 110 * g.beatPulse));
                SDL_Rect gloss{x + 4, doctrineY + 4, doctrineCellW - 8, 3};
                SDL_RenderFillRect(renderer, &gloss);
                if (g.nuclearMacroCursor == i) {
                    SDL_SetRenderDrawColor(renderer, 255, 245, 170, 255);
                    SDL_Rect sel{x + 1, doctrineY + 1, doctrineCellW - 2, 42};
                    SDL_RenderDrawRect(renderer, &sel);
                }
            }
            DrawText(renderer, doctrineX, doctrineY - 20, 1, {255, 170, 170}, "LAUNCH DOCTRINE WINDOWS");

            const int stripY = arenaY + 190;
            const int laneW = doctrineW / 8;
            for (int lane = 0; lane < 8; ++lane) {
                const int x = doctrineX + lane * laneW;
                const float lanePulse = 0.5f + 0.5f * std::sin(g.songSeconds * 6.0f + lane * 0.5f);
                SDL_SetRenderDrawColor(renderer, 36, 16, 26, 200);
                SDL_Rect laneBg{x + 2, stripY, laneW - 4, 120};
                SDL_RenderFillRect(renderer, &laneBg);
                SDL_SetRenderDrawColor(renderer, 120, 70, 90, static_cast<uint8_t>(130 + 70 * lanePulse));
                SDL_RenderDrawRect(renderer, &laneBg);

                if (g.nuclearThreat[lane] > 0) {
                    SDL_SetRenderDrawColor(renderer, 255, 90, 90, 220);
                    SDL_Rect threat{x + 8, stripY + 94 - g.nuclearThreat[lane] * 26, laneW - 16, 18 + g.nuclearThreat[lane] * 22};
                    SDL_RenderFillRect(renderer, &threat);
                    SDL_SetRenderDrawColor(renderer, 255, 210, 210, static_cast<uint8_t>(120 + 120 * g.beatPulse));
                    SDL_Rect hot{x + 10, threat.y + 2, laneW - 20, 4};
                    SDL_RenderFillRect(renderer, &hot);
                }
                if (g.nuclearIntercept[lane]) {
                    SDL_SetRenderDrawColor(renderer, 120, 240, 255, 220);
                    SDL_Rect iv{x + 10, stripY + 10, laneW - 20, 20};
                    SDL_RenderFillRect(renderer, &iv);
                }
            }

            const int playCol = g.currentBar % 8;
            const float px = static_cast<float>(doctrineX + playCol * laneW) + beatPhase * static_cast<float>(laneW);
            for (int i = -2; i <= 2; ++i) {
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, static_cast<uint8_t>(40 + (2 - std::abs(i)) * 70 + 60 * g.beatPulse));
                SDL_RenderDrawLine(renderer, static_cast<int>(px) + i, stripY - 20, static_cast<int>(px) + i, stripY + 122);
            }

            const int cX = doctrineX + g.nuclearMicroCursor * laneW + laneW / 2;
            SDL_SetRenderDrawColor(renderer, 120, 235, 255, 255);
            SDL_RenderDrawLine(renderer, cX, stripY - 26, cX, stripY + 130);
            DrawText(renderer, cX - 8, stripY - 38, 1, {120, 235, 255}, "P2");
            SDL_SetRenderDrawColor(renderer, 120, 235, 255, static_cast<uint8_t>(50 + 90 * g.beatPulse));
            SDL_Rect cursorGlow{cX - 7, stripY - 8, 14, 118};
            SDL_RenderFillRect(renderer, &cursorGlow);

            // Chaotic tactical vectors.
            for (int i = 0; i < 16; ++i) {
                const float a = g.songSeconds * 0.7f + i * 0.4f;
                const int x1 = doctrineX + static_cast<int>(std::sin(a) * (doctrineW / 2 - 40)) + doctrineW / 2;
                const int y1 = stripY + static_cast<int>(std::cos(a * 1.4f) * 26.0f);
                const int x2 = doctrineX + static_cast<int>(std::sin(a + 1.3f) * (doctrineW / 2 - 40)) + doctrineW / 2;
                const int y2 = stripY + 108 + static_cast<int>(std::cos(a * 1.1f) * 22.0f);
                SDL_SetRenderDrawColor(renderer, 255, 100, 170, static_cast<uint8_t>(18 + 26 * g.nuclearPsy));
                SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
            }

            DrawParticles(renderer, g);

            const int hudY = 600;
            DrawPanel(renderer, 120, hudY, kWindowW - 240, 140, {24, 8, 24}, {255, 130, 150});
            DrawText(renderer, 148, hudY + 18, 2, {255, 232, 118},
                     "ESCALATION:" + std::to_string(g.nuclearEscalation) + "%  DEVASTATION:" + std::to_string(g.nuclearDevastation) +
                     "%  STABILITY:" + std::to_string(g.nuclearStability) + "%");
            DrawText(renderer, 148, hudY + 44, 2, {123, 228, 255},
                     "BPM:" + std::to_string(static_cast<int>(std::round(g.nuclearBpm))) + "  SAVED:" + std::to_string(g.nuclearSaved) +
                     "  MISSED:" + std::to_string(g.nuclearMissed) + "  COMBO:" + std::to_string(g.nuclearCombo));
            DrawText(renderer, 148, hudY + 72, 2, {238, 244, 255},
                     "TIME:" + std::to_string(static_cast<int>(std::ceil(std::max(0.0f, g.nuclearBeatsRemaining * (60.0f / g.nuclearBpm))))) +
                     "  HOLD THE LINE BEFORE GLOBAL EXCHANGE");
            if (g.nuclearEndDelay <= 0.0f) {
                DrawText(renderer, 148, hudY + 98, 2, g.nuclearDevastation >= 80 ? RGB{255, 120, 120} : RGB{140, 255, 170},
                         (g.nuclearDevastation >= 80) ? "STATUS: DEFCON COLLAPSE IMMINENT" : "STATUS: DETERRENCE HOLDING");
            }

            // Beat-reactive outer warning frame.
            const uint8_t frameA = static_cast<uint8_t>(80 + 130 * g.beatPulse + 80 * g.nuclearFlash);
            SDL_SetRenderDrawColor(renderer, 255, 90, 130, frameA);
            SDL_Rect warFrame{16, 16, kWindowW - 32, kWindowH - 32};
            SDL_RenderDrawRect(renderer, &warFrame);
            SDL_Rect warFrame2{24, 24, kWindowW - 48, kWindowH - 48};
            SDL_RenderDrawRect(renderer, &warFrame2);

            if (g.nuclearEndDelay > 0.0f) {
                const int finalScore = g.nuclearSaved * 11 - g.nuclearMissed * 15 + g.nuclearStability * 2;
                DrawPanel(renderer, 360, hudY + 106, kWindowW - 720, 30, {34, 12, 28}, {255, 160, 170});
                DrawText(renderer, 384, hudY + 114, 2,
                         g.nuclearDevastation < 100 ? RGB{120, 255, 170} : RGB{255, 140, 140},
                         (g.nuclearDevastation < 100 ? std::string("DETERRENCE HELD SCORE ") : std::string("EXCHANGE TRIGGERED SCORE ")) + std::to_string(finalScore));
            }

            SDL_RenderPresent(renderer);
            continue;
        }

        if (g.mode == GameMode::SignalForge) {
            g.beatPulse = std::max(0.0f, g.beatPulse - dt * 3.2f);
            g.beatRipple = std::max(0.0f, g.beatRipple - dt * 2.0f);
            g.uiFlash = std::max(0.0f, g.uiFlash - dt * 2.0f);
            g.railFlash = std::max(0.0f, g.railFlash - dt * 2.4f);

            const float targetSignalBpm = std::min(136.0f, 112.0f + static_cast<float>(g.signalPower) * 0.18f);
            g.signalBpm += (targetSignalBpm - g.signalBpm) * std::min(1.0f, dt * 2.0f);

            const float signalStepDur = 60.0f / g.signalBpm / 4.0f;
            float secSinceStep = static_cast<float>(now - g.signalStepStartTicks) / 1000.0f;
            while (!g.paused && secSinceStep >= signalStepDur) {
                secSinceStep -= signalStepDur;
                g.signalStepStartTicks += static_cast<uint32_t>(signalStepDur * 1000.0f);
                g.currentStep = (g.currentStep + 1) % 16;
                TriggerSignalGrooveStep(g, audio);
                if (g.currentStep % 4 == 0) g.beatPulse = 1.0f;
            }

            const float signalBeatDur = 60.0f / g.signalBpm;
            float secSinceBeat = static_cast<float>(now - g.signalBeatStartTicks) / 1000.0f;
            while (!g.paused && secSinceBeat >= signalBeatDur) {
                secSinceBeat -= signalBeatDur;
                g.signalBeatStartTicks += static_cast<uint32_t>(signalBeatDur * 1000.0f);
                g.beatPulse = 1.0f;
                g.beatRipple = 1.0f;
                g.currentBar++;
                if (g.signalBeatsRemaining > 0) g.signalBeatsRemaining--;

                auto flow = EvaluateSignalCircuit(g);
                g.signalConnected = flow.first;
                const int poweredCount = flow.second;
                if (g.signalConnected) {
                    g.signalPower = std::min(100, g.signalPower + 7 + poweredCount / 10);
                    g.signalClean += 2;
                    g.signalCombo++;
                    g.signalInterference = std::max(0.0f, g.signalInterference - 0.03f);
                    TriggerVoice(audio, JuceAudioEngine::VoiceType::Sine, MidiToFreq(72), 0.06f, 28.0f, 0.07f);
                    SpawnBurst(g, static_cast<float>(kRailArenaX + kRailArenaW - 86), static_cast<float>(kRailArenaY + 2 * ((kRailArenaH - 110) / kSignalRows) + 88),
                              {120, 255, 190}, 10);
                } else {
                    g.signalPower = std::max(0, g.signalPower - 6);
                    g.signalNoise += 2;
                    g.signalCombo = 0;
                    g.signalInterference = std::min(1.0f, g.signalInterference + 0.04f);
                    g.signalTuneLock = std::max(0, g.signalTuneLock - 1);
                }

                // Noisy carrier drifts; higher radio power narrows the drift, making tuning easier.
                const float drift = std::max(0.45f, 1.8f - static_cast<float>(g.signalPower) * 0.013f);
                std::uniform_real_distribution<float> driftDist(-drift, drift);
                g.signalTargetFreq = std::clamp(g.signalTargetFreq + driftDist(g.visualRng), 0.0f, 15.0f);

                g.signalGlitchCooldown--;
                if (g.signalGlitchCooldown <= 0) {
                    std::uniform_int_distribution<int> rowDist(0, kSignalRows - 1);
                    std::uniform_int_distribution<int> colDist(0, kSignalCols - 1);
                    const int gr = rowDist(g.visualRng);
                    const int gc = colDist(g.visualRng);
                    g.signalTileRot[gr][gc] = (g.signalTileRot[gr][gc] + 1) & 3;
                    g.signalInterference = std::min(1.0f, g.signalInterference + 0.03f);
                    g.signalGlitchCooldown = std::max(2, 6 - static_cast<int>(g.signalInterference * 4.0f));
                }

                g.signalPhraseIndex++;
            }
            const float signalBeatPhase = std::clamp(secSinceBeat / std::max(0.0001f, signalBeatDur), 0.0f, 1.0f);
            const auto flowNow = EvaluateSignalCircuit(g);
            g.signalConnected = flowNow.first;

            UpdateParticles(g, dt);

            if (!g.paused && g.signalPower >= 100 && g.signalTuneLock >= 100 && g.signalEndDelay <= 0.0f) {
                g.signalEndDelay = 2.2f;
            }
            if (!g.paused && g.signalBeatsRemaining <= 0 && g.signalEndDelay <= 0.0f) {
                g.signalEndDelay = 2.8f;
            }
            if (!g.paused && g.signalEndDelay > 0.0f) {
                g.signalEndDelay -= dt;
                if (g.signalEndDelay <= 0.0f) {
                    if (!AllGameModesPlayed(g)) {
                        InitTestCardCooldown(g, RandomUnplayedMode(g));
                    } else {
                        g.running = false;
                    }
                    continue;
                }
            }

            DrawRetroRoom(renderer, kWindowW, kWindowH);
            const float psycho = std::clamp(0.75f + 0.45f * g.beatPulse + 0.45f * g.signalInterference, 0.0f, 1.0f);

            const int panelX = 80, panelY = 50, panelW = kWindowW - 160, panelH = 120;
            DrawPanel(renderer, panelX, panelY, panelW, panelH, {20, 22, 52}, {120, 214, 255});
            DrawText(renderer, panelX + 18, panelY + 20, 3, {245, 248, 255}, "SIGNAL FORGE");
            DrawText(renderer, panelX + 18, panelY + 58, 2, {255, 232, 118}, "P1 BUILD: W/A/S/D MOVE ON CIRCUIT  SPACE ROTATE TILE");
            DrawText(renderer, panelX + 18, panelY + 82, 2, {123, 228, 255}, "P2 TUNE: LEFT/RIGHT SPECTRUM  ENTER LOCK ON BEAT  BACKSPACE NEW BOARD");

            const int arenaX = 120, arenaY = 210, arenaW = kWindowW - 240, arenaH = 360;
            DrawPanel(renderer, arenaX, arenaY, arenaW, arenaH, {10, 20, 44}, {110, 190, 255});
            // Electric haze in the arena.
            for (int yb = arenaY + 8; yb < arenaY + arenaH - 8; yb += 12) {
                const float w = 0.5f + 0.5f * std::sin(g.songSeconds * 7.0f + static_cast<float>(yb) * 0.05f);
                SDL_SetRenderDrawColor(renderer, 80, 180, 255, static_cast<uint8_t>(10 + 24 * w + 26 * g.beatPulse));
                SDL_Rect band{arenaX + 6, yb, arenaW - 12, 6};
                SDL_RenderFillRect(renderer, &band);
            }

            const int cellW = (arenaW - 140) / kSignalCols;
            const int cellH = (arenaH - 110) / kSignalRows;
            const int gridX = arenaX + (arenaW - cellW * kSignalCols) / 2;
            const int gridY = arenaY + 60;
            const int gridW = cellW * kSignalCols;
            const int gridH = cellH * kSignalRows;

            for (int c = 0; c <= kSignalCols; ++c) {
                const int x = gridX + c * cellW;
                SDL_SetRenderDrawColor(renderer, 100, 140, 200, (c % 2 == 0) ? 180 : 110);
                SDL_RenderDrawLine(renderer, x, gridY, x, gridY + gridH);
            }
            for (int r = 0; r <= kSignalRows; ++r) {
                const int y = gridY + r * cellH;
                SDL_SetRenderDrawColor(renderer, 100, 140, 200, (r == 2) ? 185 : 110);
                SDL_RenderDrawLine(renderer, gridX, y, gridX + gridW, y);
            }

            const float quarterSpeedPhase = (static_cast<float>(g.currentBar % 4) + signalBeatPhase) * 0.25f;
            const float playheadXf = static_cast<float>(gridX) + quarterSpeedPhase * static_cast<float>(gridW);
            const int playheadX = static_cast<int>(std::lround(playheadXf));
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 180);
            SDL_RenderDrawLine(renderer, playheadX, gridY - 18, playheadX, gridY + gridH + 18);

            for (int r = 0; r < kSignalRows; ++r) {
                for (int c = 0; c < kSignalCols; ++c) {
                    const int x = gridX + c * cellW;
                    const int y = gridY + r * cellH;
                    SDL_SetRenderDrawColor(renderer, 10, 18, 34, 180);
                    SDL_Rect cell{x + 1, y + 1, cellW - 2, cellH - 2};
                    SDL_RenderFillRect(renderer, &cell);

                    const int mask = SignalCellMask(g, r, c);
                    const bool powered = g.signalPowered[r][c];
                    if (powered) {
                        const float pulse = 0.55f + 0.45f * std::sin(g.songSeconds * 14.0f + r * 0.8f + c * 0.6f);
                        SDL_SetRenderDrawColor(renderer, 80, 255, 190, static_cast<uint8_t>(26 + 54 * pulse + 50 * g.beatPulse));
                        SDL_Rect glow{x + 3, y + 3, cellW - 6, cellH - 6};
                        SDL_RenderFillRect(renderer, &glow);
                    }
                    const RGB wire = powered ? RGB{120, 255, 170} : RGB{115, 170, 235};
                    SDL_SetRenderDrawColor(renderer, wire.r, wire.g, wire.b, powered ? 255 : 165);
                    const int cx = x + cellW / 2;
                    const int cy = y + cellH / 2;
                    const int armX = std::max(6, cellW / 2 - 8);
                    const int armY = std::max(6, cellH / 2 - 8);
                    if ((mask & SD_N) != 0) SDL_RenderDrawLine(renderer, cx, cy, cx, cy - armY);
                    if ((mask & SD_E) != 0) SDL_RenderDrawLine(renderer, cx, cy, cx + armX, cy);
                    if ((mask & SD_S) != 0) SDL_RenderDrawLine(renderer, cx, cy, cx, cy + armY);
                    if ((mask & SD_W) != 0) SDL_RenderDrawLine(renderer, cx, cy, cx - armX, cy);
                    if (powered) {
                        SDL_SetRenderDrawColor(renderer, 220, 255, 240, static_cast<uint8_t>(180 + 70 * g.beatPulse));
                        if ((mask & SD_N) != 0) SDL_RenderDrawLine(renderer, cx + 1, cy, cx + 1, cy - armY);
                        if ((mask & SD_E) != 0) SDL_RenderDrawLine(renderer, cx, cy - 1, cx + armX, cy - 1);
                        if ((mask & SD_S) != 0) SDL_RenderDrawLine(renderer, cx - 1, cy, cx - 1, cy + armY);
                        if ((mask & SD_W) != 0) SDL_RenderDrawLine(renderer, cx, cy + 1, cx - armX, cy + 1);
                    }
                    SDL_Rect hub{cx - 3, cy - 3, 6, 6};
                    SDL_RenderFillRect(renderer, &hub);
                }
            }

            // Arc bolts across active links.
            std::uniform_real_distribution<float> boltChance(0.0f, 1.0f);
            std::uniform_int_distribution<int> jitter(-3, 3);
            for (int r = 0; r < kSignalRows; ++r) {
                for (int c = 0; c < kSignalCols; ++c) {
                    if (!g.signalPowered[r][c]) continue;
                    const int m = SignalCellMask(g, r, c);
                    const int cx = gridX + c * cellW + cellW / 2;
                    const int cy = gridY + r * cellH + cellH / 2;
                    if ((m & SD_E) != 0 && c + 1 < kSignalCols && g.signalPowered[r][c + 1] && boltChance(g.visualRng) < (0.20f + 0.35f * g.beatPulse)) {
                        const int nx = gridX + (c + 1) * cellW + cellW / 2;
                        SDL_SetRenderDrawColor(renderer, 220, 255, 255, static_cast<uint8_t>(120 + 120 * g.beatPulse));
                        SDL_RenderDrawLine(renderer, cx, cy, (cx + nx) / 2 + jitter(g.visualRng), cy + jitter(g.visualRng));
                        SDL_RenderDrawLine(renderer, (cx + nx) / 2 + jitter(g.visualRng), cy + jitter(g.visualRng), nx, cy);
                    }
                    if ((m & SD_S) != 0 && r + 1 < kSignalRows && g.signalPowered[r + 1][c] && boltChance(g.visualRng) < (0.14f + 0.26f * g.beatPulse)) {
                        const int ny = gridY + (r + 1) * cellH + cellH / 2;
                        SDL_SetRenderDrawColor(renderer, 220, 255, 255, static_cast<uint8_t>(110 + 110 * g.beatPulse));
                        SDL_RenderDrawLine(renderer, cx, cy, cx + jitter(g.visualRng), (cy + ny) / 2 + jitter(g.visualRng));
                        SDL_RenderDrawLine(renderer, cx + jitter(g.visualRng), (cy + ny) / 2 + jitter(g.visualRng), cx, ny);
                    }
                }
            }

            // Power source and radio sink.
            const int srcY = gridY + 2 * cellH + cellH / 2;
            const int sinkY = srcY;
            const int srcX = gridX - 42;
            const int sinkX = gridX + gridW + 14;
            for (int ring = 0; ring < 3; ++ring) {
                const int sw = 34 + ring * 14 + static_cast<int>(g.beatPulse * 14.0f);
                const int sh = 28 + ring * 12 + static_cast<int>(g.beatPulse * 10.0f);
                SDL_SetRenderDrawColor(renderer, 255, 225, 140, static_cast<uint8_t>(60 - ring * 12));
                SDL_Rect sr{srcX - (sw - 28) / 2, srcY - sh / 2, sw, sh};
                SDL_RenderDrawRect(renderer, &sr);
                SDL_SetRenderDrawColor(renderer, g.signalConnected ? 120 : 255, g.signalConnected ? 255 : 120, g.signalConnected ? 170 : 120,
                                       static_cast<uint8_t>(70 - ring * 14));
                SDL_Rect rr{sinkX - (sw - 32) / 2, sinkY - sh / 2, sw, sh};
                SDL_RenderDrawRect(renderer, &rr);
            }
            SDL_SetRenderDrawColor(renderer, 255, 220, 120, 230);
            SDL_Rect src{srcX, srcY - 12, 28, 24};
            SDL_RenderFillRect(renderer, &src);
            DrawText(renderer, gridX - 72, srcY - 28, 1, {255, 220, 120}, "PWR");
            SDL_SetRenderDrawColor(renderer, g.signalConnected ? 120 : 255, g.signalConnected ? 255 : 120, g.signalConnected ? 170 : 120, 230);
            SDL_Rect sink{sinkX, sinkY - 14, 32, 28};
            SDL_RenderFillRect(renderer, &sink);
            DrawText(renderer, gridX + gridW + 12, sinkY - 34, 1, {180, 235, 255}, "RADIO");

            // P1 builder cursor.
            const int p1x = gridX + g.signalP1Col * cellW;
            const int p1y = gridY + g.signalP1Row * cellH;
            SDL_SetRenderDrawColor(renderer, 255, 225, 120, 255);
            SDL_Rect p1{p1x + 3, p1y + 3, cellW - 6, cellH - 6};
            SDL_RenderDrawRect(renderer, &p1);
            DrawText(renderer, p1x + 6, p1y + 6, 1, {255, 225, 120}, "P1");

            // P2 spectrum tuner (gets easier as power rises).
            const int specX = arenaX + 90;
            const int specY = arenaY + arenaH - 54;
            const int specW = arenaW - 180;
            const int specH = 24;
            SDL_SetRenderDrawColor(renderer, 16, 22, 36, 220);
            SDL_Rect specBg{specX, specY, specW, specH};
            SDL_RenderFillRect(renderer, &specBg);
            SDL_SetRenderDrawColor(renderer, 120, 170, 230, 220);
            SDL_RenderDrawRect(renderer, &specBg);

            for (int i = 0; i < 16; ++i) {
                const float dist = std::abs(static_cast<float>(i) - g.signalTargetFreq);
                const float amp = std::max(0.0f, 1.0f - dist / std::max(1.0f, 3.8f - g.signalInterference * 2.5f));
                const int x = specX + static_cast<int>((i / 15.0f) * (specW - 1));
                const int h = static_cast<int>(amp * (specH - 4));
                const float flicker = 0.65f + 0.35f * std::sin(g.songSeconds * 18.0f + i * 0.5f);
                SDL_SetRenderDrawColor(renderer, 255, 120, 140, static_cast<uint8_t>(80 + amp * 130 * flicker + 40 * g.beatPulse));
                SDL_RenderDrawLine(renderer, x, specY + specH - 2, x, specY + specH - 2 - h);
            }

            const int targetX = specX + static_cast<int>((g.signalTargetFreq / 15.0f) * (specW - 1));
            SDL_SetRenderDrawColor(renderer, 255, 150, 180, 220);
            SDL_RenderDrawLine(renderer, targetX, specY - 6, targetX, specY + specH + 6);

            const int cursorX = specX + static_cast<int>((static_cast<float>(g.signalTuneCursor) / 15.0f) * (specW - 1));
            SDL_SetRenderDrawColor(renderer, 120, 235, 255, 255);
            SDL_RenderDrawLine(renderer, cursorX, specY - 8, cursorX, specY + specH + 8);
            DrawText(renderer, cursorX - 8, specY - 20, 1, {120, 235, 255}, "P2");

            SDL_SetRenderDrawColor(renderer, 120, 235, 255, static_cast<uint8_t>(70 + 90 * g.beatPulse));
            SDL_Rect cursorGlow{cursorX - 5, specY - 2, 10, specH + 4};
            SDL_RenderFillRect(renderer, &cursorGlow);

            const float tolBins = 0.7f + static_cast<float>(g.signalPower) * 0.035f;
            const int tolPx = static_cast<int>((tolBins / 15.0f) * specW);
            SDL_SetRenderDrawColor(renderer, 120, 255, 190, 70);
            SDL_Rect tol{targetX - tolPx, specY + 1, tolPx * 2, specH - 2};
            SDL_RenderFillRect(renderer, &tol);

            DrawParticles(renderer, g);

            const int hudY = 600;
            DrawPanel(renderer, 120, hudY, kWindowW - 240, 140, {12, 24, 52}, {96, 170, 255});
            DrawText(renderer, 148, hudY + 18, 2, {255, 232, 118},
                     "CLEAN:" + std::to_string(g.signalClean) + "  NOISE:" + std::to_string(g.signalNoise) +
                     "  FLOW COMBO:" + std::to_string(g.signalCombo));
            DrawText(renderer, 148, hudY + 44, 2, {123, 228, 255},
                     "BPM:" + std::to_string(static_cast<int>(std::round(g.signalBpm))) +
                     "  INTERFERENCE:" + std::to_string(static_cast<int>(std::round(g.signalInterference * 100.0f))) +
                     "%  RADIO POWER:" + std::to_string(g.signalPower) + "%  TUNE LOCK:" + std::to_string(g.signalTuneLock) + "%");
            DrawText(renderer, 148, hudY + 72, 2, {238, 244, 255},
                     "TIME:" + std::to_string(static_cast<int>(std::ceil(std::max(0.0f, g.signalBeatsRemaining * (60.0f / g.signalBpm))))) +
                     "  KEEP CURRENT FLOWING WHILE P2 HOLDS THE CARRIER");
            DrawText(renderer, 148, hudY + 98, 2, {140, 255, 170},
                     std::string("STATUS: ") + (g.signalConnected ? "CIRCUIT CLOSED" : "OPEN CIRCUIT") +
                     "  TUNE STREAK:" + std::to_string(g.signalTuneStreak));

            if (g.signalEndDelay > 0.0f) {
                const int finalScore = g.signalClean * 8 - g.signalNoise * 6 + g.signalPower * 3;
                DrawText(renderer, kWindowW / 2 - 170, hudY + 100, 3,
                         (g.signalPower >= 100 && g.signalTuneLock >= 100) ? RGB{120, 255, 170} : RGB{255, 170, 130},
                         ((g.signalPower >= 100 && g.signalTuneLock >= 100) ? std::string("SIGNAL LOCKED ") : std::string("NO LOCK ")) + std::to_string(finalScore));
            }

            SDL_RenderPresent(renderer);
            continue;
        }

        if (g.mode == GameMode::RailSignalRush) {
            g.beatPulse = std::max(0.0f, g.beatPulse - dt * 3.2f);
            g.beatRipple = std::max(0.0f, g.beatRipple - dt * 1.9f);
            g.uiFlash = std::max(0.0f, g.uiFlash - dt * 1.9f);
            g.railFlash = std::max(0.0f, g.railFlash - dt * 2.6f);

            const float throughputNet = static_cast<float>(std::max(0, g.railThroughput - g.railCollisions));
            const float targetRailBpm = std::min(168.0f, 112.0f + throughputNet * 0.225f);
            g.railBpm += (targetRailBpm - g.railBpm) * std::min(1.0f, dt * 2.5f);

            const float railStepDur = 60.0f / g.railBpm / 4.0f;
            float secSinceStep = static_cast<float>(now - g.railStepStartTicks) / 1000.0f;
            while (!g.paused && secSinceStep >= railStepDur) {
                secSinceStep -= railStepDur;
                g.railStepStartTicks += static_cast<uint32_t>(railStepDur * 1000.0f);
                g.currentStep = (g.currentStep + 1) % 16;
                TriggerRailGrooveStep(g, audio);
                if (g.currentStep % 4 == 0) {
                    g.beatPulse = 1.0f;
                    g.beatRipple = 1.0f;
                } else {
                    g.beatPulse = std::max(g.beatPulse, 0.45f);
                }
            }

            const float railBeatDur = 60.0f / g.railBpm;
            float secSinceBeat = static_cast<float>(now - g.railBeatStartTicks) / 1000.0f;
            int railBeatsAdvanced = 0;
            while (!g.paused && secSinceBeat >= railBeatDur) {
                secSinceBeat -= railBeatDur;
                g.railBeatStartTicks += static_cast<uint32_t>(railBeatDur * 1000.0f);
                railBeatsAdvanced++;
                if (g.railBeatsRemaining > 0) g.railBeatsRemaining--;
                g.currentBar++;
                g.beatPulse = 1.0f;
                g.beatRipple = 1.0f;

                // Hard resync of 16th clock to beat boundary to keep timing phase-locked.
                g.currentStep = (g.currentBar % 4) * 4;
                g.railStepStartTicks = g.railBeatStartTicks;

                const int dir = (g.railSpawnFlip++ % 2 == 0) ? 1 : -1;
                SpawnRailTrain(g, dir);
                if (g.railBpm > 142.0f && (g.currentBar % 3 == 0)) {
                    SpawnRailTrain(g, -dir);
                }

                // Beat burst snapped to exact grid node.
                const int beatCell = g.currentBar % (kRailGridMaxCell + 1);
                const int beatLane = g.currentBar % 5;
                const float burstX = RailCellX(static_cast<float>(beatCell));
                const float burstY = static_cast<float>(RailLaneCenterY(beatLane));
                SpawnBurst(g, burstX, burstY, HsvToRgb(std::fmod(g.songSeconds * 0.17f, 1.0f), 1.0f, 1.0f), 32);
            }

            for (int i = 0; i < railBeatsAdvanced; ++i) {
                AdvanceRailBeat(g, audio);
            }
            const float railBeatPhase = std::clamp(secSinceBeat / std::max(0.0001f, railBeatDur), 0.0f, 1.0f);
            UpdateParticles(g, dt);

            if (!g.paused && g.railBeatsRemaining <= 0 && g.railEndDelay <= 0.0f) {
                g.railEndDelay = 2.8f;
            }
            if (!g.paused && g.railEndDelay > 0.0f) {
                g.railEndDelay -= dt;
                if (g.railEndDelay <= 0.0f) {
                    if (!AllGameModesPlayed(g)) {
                        InitTestCardCooldown(g, RandomUnplayedMode(g));
                    } else {
                        g.running = false;
                    }
                    continue;
                }
            }

            DrawRetroRoom(renderer, kWindowW, kWindowH);
            const float psycho = std::clamp(0.85f + 0.55f * g.beatPulse + 0.18f * g.railFlash, 0.0f, 1.0f);

            const int panelX = 80, panelY = 50, panelW = kWindowW - 160, panelH = 120;
            DrawPanel(renderer, panelX, panelY, panelW, panelH, {16, 29, 60}, {96, 170, 255});
            DrawText(renderer, panelX + 18, panelY + 20, 3, {245, 248, 255}, "RAIL SIGNAL RUSH");
            DrawText(renderer, panelX + 18, panelY + 58, 2, {255, 232, 118}, "P1 MICRO: LEFT/RIGHT SELECT  UP/DOWN TOGGLE FLIP/NORM");
            DrawText(renderer, panelX + 18, panelY + 82, 2, {123, 228, 255}, "P2 MACRO: A/D SELECT JUNCTION  W=UP  S=STRAIGHT");

            const int arenaX = kRailArenaX, arenaY = kRailArenaY, arenaW = kRailArenaW, arenaH = kRailArenaH;
            DrawPanel(renderer, arenaX, arenaY, arenaW, arenaH, {12, 24, 42}, {90, 210, 230});

            // Grid-aligned psychedelic overlays (columns + lanes + nodes only).
            for (int cell = kRailGridMinCell; cell <= kRailGridMaxCell; ++cell) {
                const int cx = static_cast<int>(std::round(RailCellX(static_cast<float>(cell))));
                const float phase = std::fmod(g.songSeconds * 0.32f + cell * 0.21f, 1.0f);
                const RGB c = HsvToRgb(phase, 1.0f, 1.0f);
                SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, static_cast<uint8_t>(16 + 38 * psycho));
                SDL_Rect colGlow{cx - 14, arenaY + 24, 28, arenaH - 48};
                SDL_RenderFillRect(renderer, &colGlow);
            }
            for (int lane = 0; lane < 5; ++lane) {
                const int yc = RailLaneCenterY(lane);
                const float phase = std::fmod(g.songSeconds * 0.28f + lane * 0.17f, 1.0f);
                const RGB c = HsvToRgb(phase + 0.35f, 1.0f, 1.0f);
                SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, static_cast<uint8_t>(14 + 34 * psycho));
                SDL_Rect laneGlow{kRailTrackLeftX, yc - 10, kRailTrackRightX - kRailTrackLeftX + 1, 20};
                SDL_RenderFillRect(renderer, &laneGlow);
            }

            // Uniform 4x5 major grid (columns x lanes).
            for (int cell = kRailGridMinCell; cell <= kRailGridMaxCell; ++cell) {
                const int cx = static_cast<int>(std::round(RailCellX(static_cast<float>(cell))));
                SDL_SetRenderDrawColor(renderer, 110, 150, 196, 170);
                SDL_RenderDrawLine(renderer, cx, arenaY + 30, cx, arenaY + arenaH - 28);
            }

            for (int lane = 0; lane < 5; ++lane) {
                const int yc = RailLaneCenterY(lane);
                SDL_SetRenderDrawColor(renderer, 110, 150, 196, 170);
                SDL_RenderDrawLine(renderer, kRailTrackLeftX, yc - 6, kRailTrackRightX, yc - 6);
                SDL_SetRenderDrawColor(renderer, 110, 150, 196, 170);
                SDL_RenderDrawLine(renderer, kRailTrackLeftX, yc + 6, kRailTrackRightX, yc + 6);
            }

            // Beat-cell overlay once (bottom), centered to each logic cell.
            for (int cell = kRailGridMinCell; cell <= kRailGridMaxCell; ++cell) {
                const int cx = static_cast<int>(std::round(RailCellX(static_cast<float>(cell))));
                const std::string label = std::to_string(cell);
                const int textW = static_cast<int>(label.size()) * 6;
                DrawText(renderer, cx - textW / 2, arenaY + arenaH - 72, 1, {120, 150, 190}, label);
            }

            for (int j = 0; j < 4; ++j) {
                const int x = RailJunctionX(j);
                const bool macroSel = (g.railMacroCursor == j);
                const bool microSel = (g.railMicroCursor == j);
                const RGB jc = macroSel ? RGB{255, 235, 130} : RGB{170, 220, 255};
                SDL_SetRenderDrawColor(renderer, jc.r, jc.g, jc.b, 210);
                SDL_RenderDrawLine(renderer, x, arenaY + 20, x, arenaY + arenaH - 20);
                for (int lane = 0; lane < 5; ++lane) {
                    const int yc = RailLaneCenterY(lane);
                    SDL_SetRenderDrawColor(renderer, 160, 208, 236, 180);
                    SDL_Rect node{x - 3, yc - 3, 6, 6};
                    SDL_RenderFillRect(renderer, &node);
                }

                const char* m = (g.railMacroDelta[j] < 0) ? "UP" : (g.railMacroDelta[j] > 0) ? "DOWN" : "STRAIGHT";
                DrawText(renderer, x - 28, arenaY + arenaH - 46, macroSel ? 2 : 1, {255, 232, 118}, m);
                DrawText(renderer, x - 16, arenaY + 14, 2, g.railMicroFlip[j] ? RGB{120, 255, 170} : RGB{255, 150, 150},
                         g.railMicroFlip[j] ? "FLIP" : "NORM");
                if (microSel) {
                    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 220);
                    SDL_Rect c{x - 20, arenaY + 8, 40, 20};
                    SDL_RenderDrawRect(renderer, &c);
                }
            }

            for (const auto& t : g.railTrains) {
                const int y = RailLaneCenterY(t.lane);
                // Render from current beat state toward next beat so visuals stay phase-aligned
                // with beat-quantized collision logic.
                const float drawCell = static_cast<float>(t.cell) + static_cast<float>(t.dir) * railBeatPhase;
                const float drawXf = RailCellX(drawCell);
                const RGB glowA = HsvToRgb(std::fmod(g.songSeconds * 0.21f + t.lane * 0.09f, 1.0f), 1.0f, 1.0f);
                const RGB glowB = HsvToRgb(std::fmod(g.songSeconds * 0.21f + 0.5f + t.lane * 0.09f, 1.0f), 1.0f, 1.0f);
                // Chromatic ghost trails.
                SDL_SetRenderDrawColor(renderer, glowA.r, glowA.g, glowA.b, static_cast<uint8_t>(85 + 80 * g.beatPulse));
                SDL_Rect ghostA{static_cast<int>(std::round(drawXf)) - 22, y - 12, 36, 20};
                SDL_RenderFillRect(renderer, &ghostA);
                SDL_SetRenderDrawColor(renderer, glowB.r, glowB.g, glowB.b, static_cast<uint8_t>(70 + 70 * g.beatPulse));
                SDL_Rect ghostB{static_cast<int>(std::round(drawXf)) - 14, y - 8, 36, 20};
                SDL_RenderFillRect(renderer, &ghostB);
                SDL_SetRenderDrawColor(renderer, t.color.r, t.color.g, t.color.b, 230);
                SDL_Rect body{static_cast<int>(std::round(drawXf)) - 18, y - 10, 36, 20};
                SDL_RenderFillRect(renderer, &body);
                SDL_SetRenderDrawColor(renderer, 245, 245, 255, 220);
                SDL_RenderDrawRect(renderer, &body);
            }
            // Keep strict grid-locked sparkle while restoring the freer particle motion layer.
            DrawParticles(renderer, g);
            DrawRailGridLockedParticles(renderer, g);

            const int hudY = 600;
            DrawPanel(renderer, 120, hudY, kWindowW - 240, 140, {14, 24, 52}, {96, 154, 250});
            DrawText(renderer, 148, hudY + 18, 2, {255, 232, 118},
                     "THROUGHPUT:" + std::to_string(g.railThroughput) + "  COLLISIONS:" + std::to_string(g.railCollisions));
            DrawText(renderer, 148, hudY + 44, 2, {123, 228, 255},
                     "BPM:" + std::to_string(static_cast<int>(std::round(g.railBpm))) +
                         "  TRAINS LIVE:" + std::to_string(static_cast<int>(g.railTrains.size())));
            DrawText(renderer, 148, hudY + 72, 2, {238, 244, 255},
                     "TIME:" + std::to_string(static_cast<int>(std::ceil(std::max(0.0f, g.railBeatsRemaining * (60.0f / g.railBpm))))) +
                         "  KEEP LINES FLOWING, AVOID HEAD-ONS");
            DrawText(renderer, 148, hudY + 98, 2, {140, 255, 170},
                     "P1 PLANS ROUTE; P2 FLIPS JUNCTION LOGIC ON TICK");

            if (g.railEndDelay > 0.0f) {
                const int finalScore = g.railThroughput * 12 - g.railCollisions * 18;
                DrawText(renderer, kWindowW / 2 - 140, hudY + 100, 3, {255, 170, 130},
                         "RESULT " + std::to_string(finalScore));
            }

            SDL_RenderPresent(renderer);
            continue;
        }

        if (g.mode == GameMode::DuelArena) {
            g.beatPulse = std::max(0.0f, g.beatPulse - dt * 3.4f);
            g.beatRipple = std::max(0.0f, g.beatRipple - dt * 2.4f);
            g.p1Flash = std::max(0.0f, g.p1Flash - dt * 2.0f);
            g.p2Flash = std::max(0.0f, g.p2Flash - dt * 2.0f);
            g.p1HitBurst = std::max(0.0f, g.p1HitBurst - dt * 3.2f);
            g.p2HitBurst = std::max(0.0f, g.p2HitBurst - dt * 3.2f);
            g.duelBuild = std::max(0.0f, g.duelBuild - dt * 0.035f);

            // Tempo ramps up as total health drops.
            const float hpRatio = std::clamp((g.p1Hp + g.p2Hp) / 2000.0f, 0.0f, 1.0f);
            const float targetDuelBpm = 128.0f + (1.0f - hpRatio) * 26.0f;  // up to ~154 BPM (half ramp speed)
            g.duelBpm += (targetDuelBpm - g.duelBpm) * std::min(1.0f, dt * 4.0f);

            const float stepDur = 60.0f / g.duelBpm / 4.0f;
            float secSinceStep = static_cast<float>(now - g.duelStepStartTicks) / 1000.0f;
            while (!g.paused && secSinceStep >= stepDur) {
                secSinceStep -= stepDur;
                g.duelStepStartTicks += static_cast<uint32_t>(stepDur * 1000.0f);
                g.duelStep = (g.duelStep + 1) % 16;
                TriggerDuelGrooveStep(g, audio);
            }

            const float beatDur = 60.0f / g.duelBpm;
            float secSinceBeat = static_cast<float>(now - g.duelBeatStartTicks) / 1000.0f;
            while (!g.paused && secSinceBeat >= beatDur) {
                secSinceBeat -= beatDur;
                g.duelBeatStartTicks += static_cast<uint32_t>(beatDur * 1000.0f);
                g.beatPulse = 1.0f;
                g.beatRipple = 1.0f;
                if (g.duelBeatsRemaining > 0) g.duelBeatsRemaining--;
                g.currentBar++;
                ResolveDuelBeat(g, audio);
            }
            UpdateParticles(g, dt);

            if (g.duelWinner == 0) {
                if (g.p1Hp <= 0 && g.p2Hp <= 0) {
                    g.duelWinner = (g.p1Meter >= g.p2Meter) ? 1 : 2;
                } else if (g.p1Hp <= 0) {
                    g.duelWinner = 2;
                } else if (g.p2Hp <= 0) {
                    g.duelWinner = 1;
                }
                else if (g.duelBeatsRemaining <= 0) g.duelWinner = (g.p1Hp >= g.p2Hp) ? 1 : 2;
                if (g.duelWinner != 0) {
                    g.duelEndDelay = 3.0f;
                    g.duelOutcomeAge = 0.0f;
                }
            } else if (!g.paused) {
                g.duelOutcomeAge += dt;
                g.duelEndDelay -= dt;
                if (g.duelEndDelay <= 0.0f) {
                    if (!AllGameModesPlayed(g)) InitTestCardCooldown(g, RandomUnplayedMode(g));
                    else g.running = false;
                    continue;
                }
            }

            DrawRetroRoom(renderer, kWindowW, kWindowH);
            const float psycho = std::clamp(0.65f + 0.45f * g.beatPulse + 0.15f * (g.duelWinner == 0 ? 0.0f : 1.0f), 0.0f, 1.0f);
            const float rainbowPhase = std::fmod(g.songSeconds * 0.46f, 1.0f);
            DrawPsychedelicWash(renderer, g.songSeconds, psycho);
            DrawNeonBeatRings(renderer, g.songSeconds, g.beatPulse, psycho);

            const RGB borderA = HsvToRgb(rainbowPhase + 0.03f, 1.0f, 1.0f);
            const int panelX = 80, panelY = 50, panelW = kWindowW - 160, panelH = 120;
            DrawPanel(renderer, panelX, panelY, panelW, panelH, {14, 20, 40}, borderA);
            DrawText(renderer, panelX + 18, panelY + 20, 3, {245, 248, 255}, "CHORD DUEL ARENA");
            DrawText(renderer, panelX + 18, panelY + 58, 2, {255, 232, 118}, "P1: A/D MOVE  W ATTACK  S BLOCK");
            DrawText(renderer, panelX + 18, panelY + 82, 2, {123, 228, 255}, "P2: LEFT/RIGHT MOVE  UP ATTACK  DOWN BLOCK");

            const int arenaX = 120, arenaY = 210, arenaW = kWindowW - 240, arenaH = 360;
            DrawPanel(renderer, arenaX, arenaY, arenaW, arenaH, {10, 18, 30}, HsvToRgb(rainbowPhase + 0.34f, 1.0f, 1.0f));
            SDL_SetRenderDrawColor(renderer, 190, 220, 255, 70);
            for (int yline = arenaY + 20; yline < arenaY + arenaH; yline += 28) SDL_RenderDrawLine(renderer, arenaX + 10, yline, arenaX + arenaW - 10, yline);

            const int centerY = arenaY + arenaH / 2;
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 130);
            SDL_RenderDrawLine(renderer, arenaX + 30, centerY, arenaX + arenaW - 30, centerY);

            const auto posToX = [&](int pos) {
                const float t = (static_cast<float>(pos) + 3.0f) / 6.0f;
                return arenaX + 90 + static_cast<int>(t * (arenaW - 180));
            };
            const int p1x = posToX(g.p1Pos);
            const int p2x = posToX(g.p2Pos);
            const int fighterWBase = 46, fighterHBase = 72;
            const float beatScale = 1.0f + 0.16f * g.beatPulse;
            const int fighterW = static_cast<int>(std::round(fighterWBase * beatScale));
            const int fighterH = static_cast<int>(std::round(fighterHBase * beatScale));

            const bool p1IsLoser = (g.duelWinner == 2);
            const bool p2IsLoser = (g.duelWinner == 1);
            const float loseFade = std::clamp(1.0f - g.duelOutcomeAge / 1.0f, 0.0f, 1.0f);
            const float p1Fade = p1IsLoser ? loseFade : 1.0f;
            const float p2Fade = p2IsLoser ? loseFade : 1.0f;

            SDL_SetRenderDrawColor(renderer, 255, 126, 126,
                                   static_cast<uint8_t>((180 + g.p1Flash * 70) * p1Fade));
            const int p1BaseY = centerY - 6;
            SDL_Rect p1Rect{p1x - fighterW / 2, p1BaseY - fighterH, fighterW, fighterH};
            SDL_RenderFillRect(renderer, &p1Rect);
            SDL_SetRenderDrawColor(renderer, 255, 230, 160, static_cast<uint8_t>(255 * p1Fade));
            SDL_RenderDrawRect(renderer, &p1Rect);

            SDL_SetRenderDrawColor(renderer, 126, 220, 255,
                                   static_cast<uint8_t>((180 + g.p2Flash * 70) * p2Fade));
            const int p2BaseY = centerY + 6;
            SDL_Rect p2Rect{p2x - fighterW / 2, p2BaseY, fighterW, fighterH};
            SDL_RenderFillRect(renderer, &p2Rect);
            SDL_SetRenderDrawColor(renderer, 220, 255, 255, static_cast<uint8_t>(255 * p2Fade));
            SDL_RenderDrawRect(renderer, &p2Rect);

            if (g.pendingP1HitBursts > 0) {
                for (int i = 0; i < g.pendingP1HitBursts; ++i) {
                    SpawnBurst(g, static_cast<float>(p1x), static_cast<float>(p1Rect.y + fighterH / 2), {255, 120, 120}, 30);
                    SpawnBurst(g, static_cast<float>(p1x), static_cast<float>(p1Rect.y + fighterH / 2), {255, 230, 150}, 14);
                }
                g.pendingP1HitBursts = 0;
            }
            if (g.pendingP2HitBursts > 0) {
                for (int i = 0; i < g.pendingP2HitBursts; ++i) {
                    SpawnBurst(g, static_cast<float>(p2x), static_cast<float>(p2Rect.y + fighterH / 2), {120, 230, 255}, 30);
                    SpawnBurst(g, static_cast<float>(p2x), static_cast<float>(p2Rect.y + fighterH / 2), {210, 255, 255}, 14);
                }
                g.pendingP2HitBursts = 0;
            }

            // One-shot KO detonation for defeated player(s).
            if ((g.pendingKoExplosionMask & 1) != 0) {
                const float ex = static_cast<float>(p1x);
                const float ey = static_cast<float>(p1Rect.y + fighterH / 2);
                SpawnBigExplosion(g, ex, ey, {255, 110, 110}, {255, 240, 190});
                g.p1HitBurst = 1.0f;
                g.pendingKoExplosionMask &= ~1;
            }
            if ((g.pendingKoExplosionMask & 2) != 0) {
                const float ex = static_cast<float>(p2x);
                const float ey = static_cast<float>(p2Rect.y + fighterH / 2);
                SpawnBigExplosion(g, ex, ey, {110, 230, 255}, {220, 255, 255});
                g.p2HitBurst = 1.0f;
                g.pendingKoExplosionMask &= ~2;
            }

            if (g.p1HitBurst > 0.01f) {
                const int rw = static_cast<int>(72 + (1.0f - g.p1HitBurst) * 170);
                const int rh = static_cast<int>(36 + (1.0f - g.p1HitBurst) * 92);
                SDL_SetRenderDrawColor(renderer, 255, 120, 120, static_cast<uint8_t>(190 * g.p1HitBurst));
                SDL_Rect ring{p1x - rw / 2, p1Rect.y + fighterH / 2 - rh / 2, rw, rh};
                SDL_RenderDrawRect(renderer, &ring);
            }
            if (g.p2HitBurst > 0.01f) {
                const int rw = static_cast<int>(72 + (1.0f - g.p2HitBurst) * 170);
                const int rh = static_cast<int>(36 + (1.0f - g.p2HitBurst) * 92);
                SDL_SetRenderDrawColor(renderer, 120, 230, 255, static_cast<uint8_t>(190 * g.p2HitBurst));
                SDL_Rect ring{p2x - rw / 2, p2Rect.y + fighterH / 2 - rh / 2, rw, rh};
                SDL_RenderDrawRect(renderer, &ring);
            }

            DrawText(renderer, p1x - 36, p1Rect.y - 22, 2, {255, 220, 140}, DuelActionName(g.p1Queued));
            DrawText(renderer, p2x - 36, p2Rect.y + fighterH + 8, 2, {130, 238, 255}, DuelActionName(g.p2Queued));
            DrawParticles(renderer, g);

            const int hudY = 600;
            DrawPanel(renderer, 120, hudY, kWindowW - 240, 140, {12, 20, 44}, HsvToRgb(rainbowPhase + 0.7f, 1.0f, 1.0f));

            // Beat-reactive health bars.
            const int barW = 430;
            const int barH = 12;
            const int p1BarX = 148;
            const int p2BarX = kWindowW - 148 - barW;
            const int barY = hudY + 120;
            const float p1HpRatio = std::clamp(g.p1Hp / 1000.0f, 0.0f, 1.0f);
            const float p2HpRatio = std::clamp(g.p2Hp / 1000.0f, 0.0f, 1.0f);
            const float beatGlow = 0.65f + 0.35f * g.beatPulse;
            const RGB p1HpCol = HsvToRgb(0.00f + 0.15f * p1HpRatio, 1.0f, 1.0f);
            const RGB p2HpCol = HsvToRgb(0.58f + 0.10f * p2HpRatio, 1.0f, 1.0f);

            SDL_SetRenderDrawColor(renderer, 30, 30, 40, 230);
            SDL_Rect p1Bg{p1BarX, barY, barW, barH};
            SDL_Rect p2Bg{p2BarX, barY, barW, barH};
            SDL_RenderFillRect(renderer, &p1Bg);
            SDL_RenderFillRect(renderer, &p2Bg);
            SDL_SetRenderDrawColor(renderer, p1HpCol.r, p1HpCol.g, p1HpCol.b, static_cast<uint8_t>(200 * beatGlow));
            SDL_Rect p1Fill{p1BarX + 1, barY + 1, static_cast<int>((barW - 2) * p1HpRatio), barH - 2};
            SDL_RenderFillRect(renderer, &p1Fill);
            SDL_SetRenderDrawColor(renderer, p2HpCol.r, p2HpCol.g, p2HpCol.b, static_cast<uint8_t>(200 * beatGlow));
            SDL_Rect p2Fill{p2BarX + 1, barY + 1, static_cast<int>((barW - 2) * p2HpRatio), barH - 2};
            SDL_RenderFillRect(renderer, &p2Fill);
            SDL_SetRenderDrawColor(renderer, 240, 240, 255, 220);
            SDL_RenderDrawRect(renderer, &p1Bg);
            SDL_RenderDrawRect(renderer, &p2Bg);

            DrawText(renderer, 148, hudY + 18, 2, {255, 230, 140},
                     "P1 HP:" + std::to_string(g.p1Hp) + "  GUARD:" + std::to_string(g.p1Guard) + "  METER:" + std::to_string(g.p1Meter));
            DrawText(renderer, 148, hudY + 44, 2, {130, 238, 255},
                     "P2 HP:" + std::to_string(g.p2Hp) + "  GUARD:" + std::to_string(g.p2Guard) + "  METER:" + std::to_string(g.p2Meter));
            DrawText(renderer, 148, hudY + 72, 2, {238, 244, 255},
                     "TIME:" + std::to_string(static_cast<int>(std::ceil(std::max(0.0f, g.duelBeatsRemaining * (60.0f / g.duelBpm))))) +
                         "  BEAT-LOCKED ACTIONS ONLY");
            DrawText(renderer, 760, hudY + 18, 2, {255, 232, 118},
                     "MUSIC BUILD:" + std::to_string(static_cast<int>(g.duelBuild * 100.0f)));
            DrawText(renderer, 760, hudY + 44, 2, {255, 232, 118},
                     "BPM:" + std::to_string(static_cast<int>(std::round(g.duelBpm))));
            const float beatDurNow = 60.0f / g.duelBpm;
            const float secInBeatNow = std::fmod(static_cast<float>(now - g.duelBeatStartTicks) / 1000.0f, beatDurNow);
            const float distNow = std::min(secInBeatNow, beatDurNow - secInBeatNow);
            const bool beatOpen = distNow <= kDuelOnBeatWindowSec;

            // Strong beat indicator: timeline + beacon + beat-window zones.
            const int btX = 450;
            const int btY = hudY + 88;
            const int btW = 430;
            const int btH = 20;
            const float beatPhase = std::clamp(secInBeatNow / beatDurNow, 0.0f, 1.0f);
            const float winFrac = std::clamp(kDuelOnBeatWindowSec / beatDurNow, 0.0f, 0.49f);

            SDL_SetRenderDrawColor(renderer, 26, 26, 34, 240);
            SDL_Rect btBg{btX, btY, btW, btH};
            SDL_RenderFillRect(renderer, &btBg);
            SDL_SetRenderDrawColor(renderer, 210, 210, 230, 180);
            SDL_RenderDrawRect(renderer, &btBg);

            // Left and right on-beat windows.
            const int winW = static_cast<int>(btW * winFrac);
            SDL_SetRenderDrawColor(renderer, 90, 255, 150, beatOpen ? 160 : 80);
            SDL_Rect leftWin{btX + 1, btY + 1, std::max(2, winW), btH - 2};
            SDL_Rect rightWin{btX + btW - std::max(2, winW) - 1, btY + 1, std::max(2, winW), btH - 2};
            SDL_RenderFillRect(renderer, &leftWin);
            SDL_RenderFillRect(renderer, &rightWin);

            // Moving playhead.
            const int phX = btX + static_cast<int>(beatPhase * (btW - 1));
            SDL_SetRenderDrawColor(renderer, 255, 245, 180, 255);
            SDL_RenderDrawLine(renderer, phX, btY - 2, phX, btY + btH + 2);

            // Beat beacon.
            const int beaconX = btX + btW + 26;
            const int beaconY = btY + btH / 2;
            const float beaconPulse = 0.45f + 0.55f * g.beatPulse;
            const int beaconR = 6 + static_cast<int>(9.0f * beaconPulse);
            const RGB beaconCol = beatOpen ? RGB{100, 255, 155} : RGB{255, 110, 110};
            SDL_SetRenderDrawColor(renderer, beaconCol.r, beaconCol.g, beaconCol.b, 220);
            for (int a = 0; a < 360; a += 6) {
                const float rad = static_cast<float>(a) * 3.14159265f / 180.0f;
                const int x = beaconX + static_cast<int>(std::cos(rad) * beaconR);
                const int y = beaconY + static_cast<int>(std::sin(rad) * beaconR);
                SDL_RenderDrawPoint(renderer, x, y);
            }

            DrawText(renderer, 148, hudY + 98, 2, beatOpen ? RGB{120, 255, 150} : RGB{255, 150, 150},
                     beatOpen ? "BEAT WINDOW: OPEN" : "BEAT WINDOW: CLOSED");
            if (g.duelWinner != 0) {
                DrawText(renderer, kWindowW / 2 - 120, hudY + 100, 3, {255, 160, 160},
                         (g.duelWinner == 1) ? "P1 WINS" : "P2 WINS");
            }

            SDL_RenderPresent(renderer);
            continue;
        }

        if (g.mode == GameMode::TestCardCooldown) {
            if (!g.paused) {
                g.intermissionCountdown = std::max(0.0f, g.intermissionCountdown - dt);
            }
            DrawTestCardCountdown(renderer, g);
            SDL_RenderPresent(renderer);
            if (g.intermissionCountdown <= 0.0f) {
                if (g.cooldownNextMode == GameMode::DuelArena) InitDuelMode(g, now);
                else if (g.cooldownNextMode == GameMode::RailSignalRush) InitRailMode(g, now);
                else if (g.cooldownNextMode == GameMode::SignalForge) InitSignalMode(g, now);
                else if (g.cooldownNextMode == GameMode::NuclearRhythmWar) InitNuclearMode(g, now);
                else if (g.cooldownNextMode == GameMode::SnakeDuet) InitSnakeMode(g, now);
                else if (g.cooldownNextMode == GameMode::LongJumpDuet) InitLongJumpMode(g, now);
                else InitGridCoopMode(g, now);
            }
            continue;
        }

        g.uiFlash = std::max(0.0f, g.uiFlash - dt * 1.7f);
        g.beatPulse = std::max(0.0f, g.beatPulse - dt * 3.2f);
        g.comboPulse = std::max(0.0f, g.comboPulse - dt * 2.2f);
        g.macroPulse = std::max(0.0f, g.macroPulse - dt * 2.0f);
        g.beatRipple = std::max(0.0f, g.beatRipple - dt * 1.8f);

        const int ramp = static_cast<int>(g.songSeconds / kBpmRampEverySec);
        g.bpm = std::min(kStartBpm + ramp * kBpmRampAmount, kBpmMax);

        const float stepDur = StepDurationSeconds(g.bpm);
        const float barDur = BarDurationSeconds(g.bpm);
        float secSinceBarStart = static_cast<float>(now - g.barStartTicks) / 1000.0f;
        while (!g.paused && secSinceBarStart >= barDur) {
            secSinceBarStart -= barDur;
            g.barStartTicks += static_cast<uint32_t>(barDur * 1000.0f);
            ResolveBar(g);
            g.currentBar += 1;
            BeginNewBar(g);
            AdvanceTutorial(g);
        }

        int newStep = std::clamp(static_cast<int>(secSinceBarStart / stepDur), 0, 15);
        while (!g.paused && g.currentStep != newStep) {
            g.currentStep = (g.currentStep + 1) % 16;
            g.beatPulse = std::max(g.beatPulse, (g.currentStep % 4 == 0) ? 1.0f : 0.55f);
            if (g.currentStep % 4 == 0) {
                g.beatRipple = 1.0f;
                if (g.roundBeatsRemaining > 0) {
                    g.roundBeatsRemaining--;
                }
            }
            if (g.currentStep % 8 == 0) {
                g.macroPulse = 1.0f;
            }
            for (int lane = 0; lane < kMicroRows; ++lane) if (g.micro[lane][g.currentStep]) {
                TriggerLane(audio, g, lane, g.currentStep);
                g.uiFlash = std::min(1.0f, g.uiFlash + 0.04f);
                g.pendingHits.push_back({lane, g.currentStep});
            }
        }

        UpdateParticles(g, dt);

        if (!g.paused && g.roundBeatsRemaining <= 0) {
            if (!AllGameModesPlayed(g)) InitTestCardCooldown(g, RandomUnplayedMode(g));
            else g.running = false;
            continue;
        }

        DrawRetroRoom(renderer, kWindowW, kWindowH);
        const float psycho = std::clamp(0.72f + 0.42f * g.beatPulse + 0.05f * static_cast<float>(std::min(g.combo, 16)), 0.0f, 1.0f);
        const float rainbowPhase = std::fmod(g.songSeconds * 0.42f, 1.0f);
        DrawPsychedelicWash(renderer, g.songSeconds, psycho);
        DrawNeonBeatRings(renderer, g.songSeconds, g.beatPulse, psycho);

        const int margin = 42;
        const int panelGap = 20;
        const int ox = 0;
        const int oy = 0;

        if (g.beatRipple > 0.01f) {
            const int cx = kWindowW / 2 + ox;
            const int cy = kWindowH / 2 + oy;
            for (int i = 0; i < 3; ++i) {
                const float f = std::max(0.0f, g.beatRipple - i * 0.22f);
                if (f <= 0.0f) continue;
                const int rw = static_cast<int>(220 + (1.0f - f) * 520 + i * 90);
                const int rh = static_cast<int>(120 + (1.0f - f) * 280 + i * 50);
                SDL_SetRenderDrawColor(renderer, 98, 214, 255, static_cast<uint8_t>(f * 26));
                SDL_Rect pulseRect{cx - rw / 2, cy - rh / 2, rw, rh};
                SDL_RenderDrawRect(renderer, &pulseRect);
            }
        }

        const int topPanelX = margin + ox, topPanelY = 38 + oy;
        const int topPanelW = kWindowW - margin * 2, topPanelH = 138;
        const float pulseAmount = 0.55f + 0.45f * g.beatPulse;

        const RGB borderA = HsvToRgb(rainbowPhase + 0.02f, 0.92f, 1.0f);
        const RGB borderB = HsvToRgb(rainbowPhase + 0.36f, 0.92f, 1.0f);
        const RGB borderC = HsvToRgb(rainbowPhase + 0.68f, 0.92f, 1.0f);
        DrawPanel(renderer, topPanelX, topPanelY, topPanelW, topPanelH,
                  {12, 18, 44}, {static_cast<uint8_t>(borderA.r * pulseAmount), static_cast<uint8_t>(borderA.g * pulseAmount), static_cast<uint8_t>(borderA.b * pulseAmount)});

        const int gridPanelY = 196, gridPanelH = 470;
        const int gridPanelW = (kWindowW - margin * 2 - panelGap) / 2;
        const int leftPanelX = margin;
        const int rightPanelX = leftPanelX + gridPanelW + panelGap;
        DrawPanel(renderer, leftPanelX, gridPanelY, gridPanelW, gridPanelH,
                  {16, 28, 60}, {static_cast<uint8_t>(borderB.r * pulseAmount), static_cast<uint8_t>(borderB.g * pulseAmount), static_cast<uint8_t>(borderB.b * pulseAmount)});
        DrawPanel(renderer, rightPanelX, gridPanelY, gridPanelW, gridPanelH,
                  {12, 32, 52}, {static_cast<uint8_t>(borderC.r * pulseAmount), static_cast<uint8_t>(borderC.g * pulseAmount), static_cast<uint8_t>(borderC.b * pulseAmount)});

        const int bottomPanelY = 678, bottomPanelH = 104;
        DrawPanel(renderer, margin, bottomPanelY, kWindowW - margin * 2, bottomPanelH,
                  {12, 24, 50}, {static_cast<uint8_t>(borderA.r * pulseAmount), static_cast<uint8_t>(borderA.g * pulseAmount), static_cast<uint8_t>(borderA.b * pulseAmount)});

        DrawText(renderer, 62 + ox, 54 + oy, 3, {244, 246, 255}, "MULTIPLAYER MUSIC CO-OP");
        DrawText(renderer, 62 + ox, 90 + oy, 2, {255, 232, 118}, "P1 MACRO: W/S A/D SPACE");
        DrawText(renderer, 62 + ox, 112 + oy, 2, {123, 228, 255}, "P2 MICRO: ARROWS ENTER");

        const int remaining = static_cast<int>(std::ceil(std::max(0.0f, g.roundBeatsRemaining * (60.0f / g.bpm))));
        DrawText(renderer, 62 + ox, 144 + oy, 2, {246, 248, 255},
                 "SCORE:" + std::to_string(g.score) + "  COMBO:" + std::to_string(g.combo) + "  BPM:" + std::to_string(static_cast<int>(g.bpm)));
        DrawText(renderer, 760 + ox, 144 + oy, 2, {246, 248, 255},
                 "BAR:" + std::to_string(g.currentBar + 1) + " TIME:" + std::to_string(remaining) + " OBJ:" + ObjectiveName(g.objective));

        if (g.combo > 0) {
            const int cs = (g.comboPulse > 0.55f) ? 3 : 2;
            const int cw = 180 + static_cast<int>(18 * g.comboPulse);
            const int ch = 46 + static_cast<int>(8 * g.comboPulse);
            DrawPanel(renderer, kWindowW - margin - 194 + ox, topPanelY + 18, cw, ch, {34, 61, 92}, {255, 228, 116});
            DrawText(renderer, kWindowW - margin - 180 + ox, topPanelY + 31, cs, {255, 244, 178}, "X" + std::to_string(g.combo));
        }
        if (g.paused) DrawText(renderer, 62 + ox, 166 + oy, 2, {255, 130, 130}, "PAUSED (P)");

        const int macroCellW = 48, macroCellH = 48;
        const int macroX = leftPanelX + (gridPanelW - kMacroCols * macroCellW) / 2;
        const int macroY = gridPanelY + 60;
        SDL_SetRenderDrawColor(renderer, 6, 10, 20, 190);
        SDL_Rect macroBack{macroX - 10, macroY - 10, kMacroCols * macroCellW + 18, kMacroRows * macroCellH + 18};
        SDL_RenderFillRect(renderer, &macroBack);
        SDL_SetRenderDrawColor(renderer, 255, 238, 140, 230);
        SDL_RenderDrawRect(renderer, &macroBack);
        DrawText(renderer, leftPanelX + 20, gridPanelY + 18, 2, {255, 232, 118}, "MACRO GRID");
        DrawGrid(renderer, macroX, macroY, kMacroRows, kMacroCols, macroCellW, macroCellH,
                 [&](int r, int c) { return g.macroChordByBar[c] == r + 1; },
                 g.macroSelectedRow, g.macroCursorCol, g.currentBar % kMacroCols, g.macroPulse, rainbowPhase + 0.08f,
                 {16, 28, 46}, {255, 188, 64}, {255, 252, 210});

        const int microCellW = 24, microCellH = 48;
        const int microX = rightPanelX + (gridPanelW - kMicroCols * microCellW) / 2;
        const int microY = macroY;
        SDL_SetRenderDrawColor(renderer, 5, 12, 18, 190);
        SDL_Rect microBack{microX - 10, microY - 10, kMicroCols * microCellW + 18, kMicroRows * microCellH + 18};
        SDL_RenderFillRect(renderer, &microBack);
        SDL_SetRenderDrawColor(renderer, 124, 255, 255, 230);
        SDL_RenderDrawRect(renderer, &microBack);

        if (!g.pendingHits.empty()) {
            for (const auto& hit : g.pendingHits) {
                const float cx = static_cast<float>(microX + hit.step * microCellW + microCellW / 2);
                const float cy = static_cast<float>(microY + hit.lane * microCellH + microCellH / 2);
                SpawnBurst(g, cx, cy, LaneColor(hit.lane), 9);
            }
            g.pendingHits.clear();
        }

        DrawText(renderer, rightPanelX + 20, gridPanelY + 18, 2, {123, 228, 255}, "MICRO GRID");
        DrawGrid(renderer, microX, microY, kMicroRows, kMicroCols, microCellW, microCellH,
                 [&](int r, int c) { return g.micro[r][c]; },
                 g.microSelectedRow, g.microCursorCol, g.currentStep, g.beatPulse, rainbowPhase + 0.38f,
                 {10, 34, 44}, {74, 223, 255}, {223, 255, 255});

        DrawParticles(renderer, g);

        const int meterX = margin + 24 + ox, meterY = bottomPanelY + 36;
        const int meterW = kWindowW - margin * 2 - 48, meterH = 22;
        SDL_SetRenderDrawColor(renderer, 28, 36, 55, 255);
        SDL_Rect meterBg{meterX, meterY, meterW, meterH}; SDL_RenderFillRect(renderer, &meterBg);
        const float fill = (g.roundBeatsTotal > 0)
                               ? std::clamp(static_cast<float>(g.roundBeatsRemaining) / static_cast<float>(g.roundBeatsTotal), 0.0f, 1.0f)
                               : 0.0f;
        SDL_SetRenderDrawColor(renderer, 90, 255, 176, 255);
        SDL_Rect meterFill{meterX + 2, meterY + 2, static_cast<int>((meterW - 4) * fill), meterH - 4}; SDL_RenderFillRect(renderer, &meterFill);
        const int shimmerX = meterX + 2 + static_cast<int>((meterW - 12) * std::fmod(g.songSeconds * 0.7f, 1.0f));
        SDL_SetRenderDrawColor(renderer, 220, 255, 235, 70);
        SDL_Rect shimmer{shimmerX, meterY + 2, 10, meterH - 4};
        SDL_RenderFillRect(renderer, &shimmer);
        DrawText(renderer, meterX, meterY - 22, 2, {236, 240, 255}, "ROUND TIME");

        DrawText(renderer, margin + 24 + ox, bottomPanelY + 66, 2, {255, 232, 118}, "ACTIVE CHORD:" + std::to_string(g.activeChord));
        DrawText(renderer, margin + 280 + ox, bottomPanelY + 66, 2, {216, 224, 240}, "BACKSPACE CLEAR TAB PREVIEW P PAUSE T TUTORIAL ESC QUIT");

        if (g.tutorialEnabled) {
            const int tutX = 330, tutY = 574, tutW = 700, tutH = 84;
            DrawPanel(renderer, tutX + ox, tutY + oy, tutW, tutH, {26, 42, 74}, {124, 176, 255});
            DrawText(renderer, tutX + 18 + ox, tutY + 14 + oy, 2, {255, 232, 118}, TutorialTitle(g.tutorialStep));
            DrawText(renderer, tutX + 18 + ox, tutY + 44 + oy, 2, {216, 232, 255}, TutorialDetail(g));
        } else {
            DrawText(renderer, kWindowW - 316 + ox, bottomPanelY + 76, 2, {150, 190, 220}, "TUTORIAL OFF (T)");
        }

        if (remaining <= 0) {
            DrawText(renderer, kWindowW - 300 + ox, bottomPanelY + 44 + oy, 3, {255, 120, 120}, "ROUND END");
            DrawText(renderer, kWindowW - 300 + ox, bottomPanelY + 78 + oy, 2, {245, 245, 245}, "FINAL SCORE:" + std::to_string(g.score));
        }

        if (g.uiFlash > 0.001f) {
            SDL_SetRenderDrawColor(renderer, 120, 210, 255, static_cast<uint8_t>(g.uiFlash * 45.0f));
            SDL_Rect fr{0, 0, kWindowW, kWindowH}; SDL_RenderFillRect(renderer, &fr);
        }
        const RGB chromaA = HsvToRgb(rainbowPhase + 0.12f, 1.0f, 1.0f);
        const RGB chromaB = HsvToRgb(rainbowPhase + 0.62f, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(renderer, chromaA.r, chromaA.g, chromaA.b, static_cast<uint8_t>(24 + 52 * psycho));
        SDL_Rect leftTint{0, 0, kWindowW / 2, kWindowH};
        SDL_RenderFillRect(renderer, &leftTint);
        SDL_SetRenderDrawColor(renderer, chromaB.r, chromaB.g, chromaB.b, static_cast<uint8_t>(24 + 52 * psycho));
        SDL_Rect rightTint{kWindowW / 2, 0, kWindowW / 2, kWindowH};
        SDL_RenderFillRect(renderer, &rightTint);

        SDL_RenderPresent(renderer);
    }

    audio.stop();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
