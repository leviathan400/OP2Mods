-- BigMap.placement.lua  -  BigMap  -  starting layout.
-- One Eden player on a big map made by ..\tools\make_bigmap.py (bigmap.map). The base and the
-- test units are made by BigMap.lua, so this file only sets up the player.
return {
  name = "BigMap", map = "bigmap.map", tech = "MULTITEK.TXT", type = "Colony",
  max_tech = 12, players_count = 1,

  players = {
    [1] = { colony = "Eden", human = true, color = "Blue",
            resources = { common_ore = 5000, rare_ore = 5000, food = 5000, kids = 8, workers = 20, scientists = 10,
                          tech_level = 12 },
            center_view = { 800, 200 } },
  },

  units = {}, beacons = {}, walls = {}, regions = {}, markers = {},
}
