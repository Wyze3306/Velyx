#pragma once

#include <cstdint>

namespace velyx::turn {

/// The player's mouse, on its way to the game and before the game sees it.
///
/// Three doors, and a machine walks through one or two of them:
///
///   * raw input off the message pump, which is how WineGDK feeds the game;
///   * GameInput readings, which is how Windows does;
///   * `LocalPlayer::applyTurnDelta`, the game's own function, where the movement has
///     already become degrees. It is the same on every machine — but it needs the pack
///     to carry its signature, where the two above need nothing at all.
///
/// The input doors hand over counts, the game door degrees. Whichever is used, the
/// handlers see degrees, because that is what every setting touching a camera is
/// written in. While the game door is standing the input doors stop shaping and only
/// empty themselves behind an interface: shaped twice, a sensitivity is squared.
///
/// Shaping belongs to whichever thread carries the mouse — the pump under Wine, the
/// game's input thread on Windows — and only ever one of them. `inject` is the one
/// entry another thread calls, and the only state that has to be atomic for its own
/// sake.

/// Runs a movement in counts past the handlers and returns what the game should be
/// told. Both arguments are counts, in and out. Left untouched while the game door is
/// standing: the movement is shaped there instead, once it has become degrees.
void shape(int64_t& x, int64_t& y);

/// The game door's own version: the game's degrees in, the handlers' degrees out.
void shapeDegrees(float& yaw, float& pitch);

/// Asks for a turn the player did not make, applied to the next movement. Added after
/// the handlers have had the event, so a scripted about-face is not scaled by whatever
/// the player set their own sensitivity multiplier to.
///
/// Through the game door the degrees are the game's own and land exactly. Through an
/// input door they are nominal — what one count is worth is the game's sensitivity
/// slider — and a caller that cares about the absolute angle carries a calibration.
void inject(float yawDegrees, float pitchDegrees);

/// Said by a hook that has seen the game actually read the mouse through it. Being
/// able to hook a door is not the same as the game walking through it, and only the
/// second one means a module can turn a camera. The door is named in the log once, so
/// which one a machine uses is a question the log answers rather than a guess.
void markCarried(const char* door);

/// Whether anything is carrying the mouse yet.
[[nodiscard]] bool carried();

/// Said by the game door when it is hooked, and taken back when it stands down. While
/// it holds, the movement is shaped there in the game's own degrees and the input
/// doors let it pass.
void markExact(bool exact);

/// Whether the game door is standing — when a degree asked for is a degree turned.
[[nodiscard]] bool exact();

/// Drops the movement owed in both directions. For a menu opening, where nothing the
/// player did and nothing a module asked for should reach the game, and for a hook
/// standing down.
void forget();

}
