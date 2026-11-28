-- BigMap.lua  -  a test mission for the MapSize module (OP2Lua mission)
-- =====================================================================
-- Runs on a big map made by ..\tools\make_bigmap.py (bounded: hard east and west edges) and
-- exercises each thing MapSize changes, far from the corner the stock game's limits cover
-- (columns past 511, rows past 255):
--
--   * a base at BASE: Command Center, Tokamak and Structure Factory, joined by tubes. The tube /
--     power flood fill decides whether the factory gets power: logged as `power:` every 5 marks
--     (MapSize's tube bitmap);
--   * Lynx sent on long moves: far west (on a bounded map they must go the long way round, not
--     across the east edge), to the east edge, south past row 256 (the pathfinder's corridor
--     rows) and, on a 1024-tall map, past row 512 (the tall waypoint layout); arrivals logged as
--     `arrive:`. A target on a map too short for it is moved onto the map by the game;
--   * the day/night band (OPU's light tables, which MapSize checks for): daylight is NOT forced
--     on, so the band moves across the whole width - check it in game, and that units are drawn
--     lit / dark with it;
--   * Blight at BLIGHT (MapSize's spread picker): its spread is left to the game.
--
-- KNOWN: OP2Lua packs a move's waypoint itself, with y in 14 bits, so the two moves past row 511
-- end up 512 rows up (760 -> 248). Test those with MapSize's debug orders (Debug=1,
-- OPU\logs\MapSize-cmd.txt: `move <unit> <x> <y>`), which go through the game's own Unit::DoMove.
--
-- Output: OPU\logs\OP2Lua-cBigMap.log.

local BASE    = { 800, 200 }    -- the base (east half: columns past 511)
local MOVES   = {               -- { from, to, count }
  { { 790, 210 }, { 100, 220 }, 3 },    -- far west: on a bounded map the long way (not across the east edge)
  { { 790, 210 }, { 1000, 230 }, 2 },   -- to the east edge (a bounded map's last columns are 1000-1007)
  { { 790, 210 }, { 820, 430 }, 3 },    -- south past row 256 (fits a 1024 x 512 map)
  { { 790, 210 }, { 790, 760 }, 3 },    -- south past row 512 (1024-tall maps: the tall waypoint layout)
  { { 790, 210 }, { 100, 1000 }, 2 },   -- to the far south-west corner (legs west, rows past 512)
}
local BLIGHT  = { 600, 300 }
local REPORT_EVERY = 5          -- marks

-- lynx: one entry per test Lynx: { unit, tx, ty (its target), order (tick of the move order),
-- arrived (tick, once there) }. cc / tok / fac: the base's Command Center, Tokamak, Structure Factory.
local lynx, cc, tok, fac = {}, nil, nil, nil

-- The open tile nearest (cx, cy) (a square spiral out to 40 tiles), so a target is never in rock.
local function open_tile(cx, cy)
  for r = 0, 40 do
    for y = cy - r, cy + r do
      for x = cx - r, cx + r do
        if math.max(math.abs(x - cx), math.abs(y - cy)) == r then
          local c = game.cell(x, y)
          if c.passable and not c.blocked and not c.lava then return x, y end
        end
      end
    end
  end
  return cx, cy
end

-- A building on a clear spot near `near`, placed exactly there (the spot is known to be free).
local function build(type, near)
  local p = game.find_place(near, type)
  if not p then print("BigMap: no place for " .. type .. " near " .. near[1] .. "," .. near[2]); return nil end
  local u = game.create_unit{ type = type, player = 1, at = { p.x, p.y }, exact = true }[1]
  print(string.format("BigMap: %s at (%d,%d)", type, p.x, p.y))
  return u
end

-- An L-shaped tube from building a to building b (along a's row, then down b's column).
local function tube(a, b)
  if not a or not b then return end
  local x1, x2 = math.min(a.x, b.x), math.max(a.x, b.x)
  local y1, y2 = math.min(a.y, b.y), math.max(a.y, b.y)
  game.create_tube{ x1, a.y, x2, a.y }
  game.create_tube{ b.x, y1, b.x, y2 }
end

-- Every REPORT_EVERY marks: whether the factory has power, then each Lynx - `arrive:` the first
-- time it is within 2 tiles of its target, else `progress:` with where it is.
local function report()
  if fac and fac.alive then
    print(string.format("power: Structure Factory enabled=%s disabled=%s (Tokamak %s)", tostring(fac.enabled),
                        tostring(fac.disabled), tok and tok.alive and "standing" or "gone"))
  end
  for _, l in ipairs(lynx) do
    local u = l.unit
    if u.alive and not l.arrived and math.max(math.abs(u.x - l.tx), math.abs(u.y - l.ty)) <= 2 then
      l.arrived = game.tick()
      print(string.format("arrive: Lynx #%d at (%d,%d) after %d ticks", u.id, u.x, u.y, l.arrived - l.order))
    elseif u.alive and not l.arrived then
      print(string.format("progress: Lynx #%d at (%d,%d) -> (%d,%d)", u.id, u.x, u.y, l.tx, l.ty))
    end
  end
end

-- The mission: base and Lynx made at once; the move orders at mark 1 (so the units are settled
-- in), the Blight at mark 2, and the report from then on.
function on_init()
  print(string.format("BigMap: OP2LuaCore %s", (game.version())))
  cc  = build("CommandCenter", BASE)
  tok = build("Tokamak", { BASE[1] + 8, BASE[2] })
  fac = build("StructureFactory", { BASE[1], BASE[2] + 8 })
  tube(cc, tok)
  tube(cc, fac)
  for _, m in ipairs(MOVES) do
    local made = game.create_unit{ type = "Lynx", player = 1, at = m[1], count = m[3], weapon = "Laser" }
    local tx, ty = open_tile(m[2][1], m[2][2])
    print(string.format("BigMap: %d Lynx from (%d,%d) to (%d,%d)", #made, m[1][1], m[1][2], tx, ty))
    for _, u in ipairs(made) do lynx[#lynx + 1] = { unit = u, tx = tx, ty = ty } end
  end
  players[1]:center_view(BASE)
  at_mark(1, function()
    for _, l in ipairs(lynx) do l.order = game.tick(); l.unit:move({ l.tx, l.ty }) end
    print(string.format("BigMap: %d Lynx ordered", #lynx))
  end)
  at_mark(2, function() game.create_blight(BLIGHT); print("BigMap: Blight at " .. BLIGHT[1] .. "," .. BLIGHT[2]) end)
  every(marks(REPORT_EVERY), report)
end
