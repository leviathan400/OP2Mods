# Plan: make op2rally multiplayer-safe

## Determinism model (why this matters)

Outpost 2 is **lockstep**. Every machine runs the identical simulation, kept in sync by exchanging
player **commands** each command-tick; a sync checksum is compared and a mismatch drops the desynced
peer ("out of sync"). Two rules follow:

- Anything that changes **simulation state** must go through the **command queue** (so all machines
  apply it at the same tick).
- Anything **local/visual** (UI text, sounds, overlays) must not touch sim state and runs only on
  the local machine.

## Audit

| Piece | Mechanism | MP-safe? |
|---|---|---|
| Auto-rally move | `Unit::DoMove` -> `ctMoMove` command | YES (lockstep command; tile is in the packet; only the owning machine issues it) |
| Rally points (`g_milRally`/`g_colRally`) | local mod state | YES (local; the tile travels inside each command) |
| Hotkey set + text message | `GetAsyncKeyState` + `MessageLog::AddMessage` | YES (local UI) |
| Circle marker | `PlaceMarker` / `DoDeath` spawn/kill a real sim unit | **NO - desync** |

The only blocker is the marker. The auto-rally is already structured to be safe.

## Phase 1 - Verify the auto-rally in real MP (highest priority, low effort)

Goal: confirm a `DoMove` issued from our per-cycle hook actually enters the networked command stream
and replays identically on the other machine (it should, since mission `Do*` calls work in MP, but
issuing from inside `ProcessGameCycle` is unusual and must be validated).

Steps:
1. Build a **no-marker** variant (compile-time flag, or just skip `PlaceMarker` when networked - see
   Phase 3) so the marker desync can't mask the result.
2. 2-player LAN test (second peer = a real OP2 install on another PC, or our `OP2SessionLogger` as
   the second client to capture the wire):
   - Both players set rally points and build vehicles.
   - Confirm each player's new units drive to that player's rally point **on both screens**.
   - Run several minutes; confirm **no desync drop**.
3. Use `OP2SessionLogger` to confirm our `ctMoMove` (0x02) actually appears on the wire when a unit
   auto-rallies (proves the hook-issued command is broadcast, not applied only locally).

Exit criteria: units rally correctly on both machines, no desync. If a command issued from the hook
does **not** broadcast (units rally only on the owner's screen, then desync), fall back to issuing
the move from a safer point (e.g. defer to the start of the next command-tick) - investigate the
command-collection path around `Player::ProcessCommandPacket` / the command-tick boundary.

## Phase 2 - Confirm the marker truly cannot be a sim unit (quick research)

Check whether marker/gaia units (`type 0x72`, owner 6) are excluded from the sync checksum **and**
whether their creation avoids shifting the deterministic unit-ID allocator. If creating one bumps the
shared unit-index counter, it desyncs even if the unit itself is not checksummed (the next real unit
gets a different id per machine). Expected result: unsafe -> proceed to Phase 3.

## Phase 3 - Make the marker MP-safe

### 3a (recommended first): single-player-only marker
- Find the "is a network/multiplayer game" global (from the netcode: the session/player-list state;
  candidate area near the command-processing / `TethysGame` net flags).
- When networked, **skip** `PlaceMarker`/`DoDeath`; keep the text message + the unit movement as
  confirmation. Fully safe, near-zero effort, no desync risk. Marker simply does not show in MP.

### 3b (full): client-side render overlay (marker in MP too)
Draw the circle ourselves each frame at the rally tile - local only, no sim object:
1. Hook OP2's per-frame map render (after the viewport background is drawn, e.g. around
   `Viewport::DrawBackground @ 0x470080`), where the map surface/HDC is available.
2. Convert rally tile -> screen pixel using the viewport scroll origin (`Viewport+0x830/0x834`,
   written by `Viewport::SetViewPos @ 0x46FD10`) and the viewport's client rect; cull if off-screen.
3. Draw the circle. OP2 uses DirectDraw, so prefer a DirectDraw-surface blit or reuse an OP2 sprite;
   plain GDI on the window may not composite over a DirectDraw surface.
4. Honor the existing 200-tick lifetime (`g_*MarkerTick`).

This is local-only -> MP-safe, and correctly shows the marker only on the owning player's screen.

Recommendation: ship 3a (safe + quick) so rally is fully MP-usable, then do 3b if a visible marker
in MP is wanted.

## Phase 4 - Final validation

Full 2-3 player match (markers per the chosen route). Confirm: both players' rallies work
independently, marker behaves (SP-marker absent or overlay present), and **no desync over a full
game**. Re-check the message log is not part of the checksum (it should be pure UI).

## Summary of effort

- Phase 1 (verify rally): small - mostly a test, reusing `OP2SessionLogger`.
- Phase 3a (SP-only marker): small - one network-game check + guard.
- Phase 3b (overlay marker): medium/large - a render hook + viewport->screen draw in DirectDraw.

Net: the rally feature is likely **already multiplayer-safe today except for the marker**; gating the
marker to single-player (3a) makes the whole module MP-safe with minimal work, and the overlay (3b)
is the optional polish to keep the marker visible in multiplayer.
