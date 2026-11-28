"""make_bigmap.py - build a large test map from a stock map.

Usage:
    python make_bigmap.py <source.map> <out.map> [width] [height] [bounded|wrap]

width and height are in tiles (powers of two, default 1024 x 1024). Maps wider than 512 are made
"bounded" by default (hard edges, like the small stock maps - needs the MapSize module): the last
SEAM columns are impassable and outside the clip rect, so nothing crosses from the east edge to the
west edge, and MapSize stops the view at the edges. "wrap" makes a map that wraps round east-west
like the stock 512-wide maps (the default at 512).

The source's tiles fill the new size: repeated across, and down its inner rows mirrored back and
forth between its own top and bottom edges (see main()). Everything after the tile grid and the
clip rect (tileset list, tile mappings, terrain types, version tags, tile groups) is copied
unchanged, so the new map uses the same tilesets as the source. Given the source's own size and
mode it writes the source back byte for byte (a check that the format is read right).

.map layout used here (see the mod's README):
    header   20 bytes: u32 version, u32 bSavedGame (0), u32 lgWidth, u32 height, u32 tilesetCount
    tiles    width * height u32 cells, stored in 32-tile-wide column strips:
             index = ((x >> 5) * height + y) * 32 + (x & 31)
    clipRect 16 bytes: x1, y1, x2, y2 (tiles)
    rest     tileset sources, mappings, terrain types, version tag(s), tile groups

Only the unit-index bits (16-26) of a cell are cleared; a .map file should have none set anyway.
"""

import struct
import sys


def read_map(path):
    """Read a .map: (version, tilesetCount, width, height, cells, clip rect, the bytes after it)."""
    data = open(path, 'rb').read()
    version, saved, lg_w, height, tilesets = struct.unpack_from('<5I', data, 0)
    if saved:
        raise SystemExit('%s is a saved game, not a map' % path)
    width = 1 << lg_w
    n = width * height
    cells = struct.unpack_from('<%dI' % n, data, 20)
    clip = struct.unpack_from('<4i', data, 20 + n * 4)
    rest = data[20 + n * 4 + 16:]
    return version, tilesets, width, height, cells, clip, rest


def cell_index(x, y, height):
    """Index of tile (x, y) in the column-strip layout."""
    return ((x >> 5) * height + y) * 32 + (x & 31)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    new_w = int(sys.argv[3]) if len(sys.argv) > 3 else 1024
    new_h = int(sys.argv[4]) if len(sys.argv) > 4 else 1024
    mode = sys.argv[5] if len(sys.argv) > 5 else ('bounded' if new_w > 512 else 'wrap')
    if mode not in ('bounded', 'wrap'):
        raise SystemExit('mode must be bounded or wrap')
    if mode == 'bounded' and new_w < 512:
        raise SystemExit('maps narrower than 512 always have hard edges (the game pads them); leave the mode out')
    lg_w = new_w.bit_length() - 1
    if (1 << lg_w) != new_w or new_h & (new_h - 1):
        raise SystemExit('width and height must be powers of two')

    version, tilesets, w, h, cells, clip, rest = read_map(src)
    print('source %s: %d x %d, clip %s' % (src, w, h, clip))

    # Fill the new grid from the source. Across: repeated (x modulo the source width; a 512-wide
    # source wraps, so the seams match). Down: the source's top border rows, then its playable rows
    # (clip y1..y2, less EDGE) mirrored back and forth, then its bottom border rows - repeating the whole
    # source would put its impassable bottom and top borders together as a wall across the map.
    # Stock maps keep a band of cliffs inside their playable rows along the edges too, so only the
    # rows EDGE in from each side are mirrored; the top and bottom EDGE rows come from the source's.
    EDGE = 16
    y1, y2 = clip[1] + EDGE, clip[3] - EDGE     # the source rows that get mirrored
    span = y2 - y1 + 1

    def src_row(y):
        """The source row that row y of the new map is copied from."""
        if new_h == h:                       # same height: row for row
            return y
        if y < y1:                           # the source's top edge, at the top
            return y
        if y > new_h - (h - y2):             # the source's bottom edge, at the bottom
            return y - (new_h - h)
        k = (y - y1) % (2 * span)            # in between: y1..y2, then back y2..y1, and so on
        return y1 + (k if k < span else 2 * span - 1 - k)
    rows = [src_row(y) for y in range(new_h)]
    out_cells = [0] * (new_w * new_h)
    for x in range(new_w):
        sx = x % w
        for y in range(new_h):
            v = cells[cell_index(sx, rows[y], h)]
            out_cells[cell_index(x, y, new_h)] = v & ~(0x7FF << 16)   # no unit index in a .map

    # The clip rect: the tiles units may use. y keeps the source's top and bottom margins (stock
    # 512-wide maps use y1 = 1 or 2, y2 = height - 2). x: a wrapping wide map has no x clip
    # (-1 .. 0x7FFFFFFF, as stock); a bounded one is clipped to all but the seam; a map narrower
    # than 512 is clipped to its real columns, which the game puts 32 tiles in (see the README).
    y1, y2 = clip[1], new_h - (h - clip[3])   # new clip rows
    if new_w >= 512 and mode == 'bounded':
        # The seam: the last SEAM columns become impassable rock (cell type 6, the type the game
        # fills its own pads with), outside the clip rect. 16 is more than any weapon's or
        # sensor's reach, so units at the two edges never see or reach each other.
        SEAM = 16
        for x in range(new_w - SEAM, new_w):
            for y in range(new_h):
                i = cell_index(x, y, new_h)
                out_cells[i] = (out_cells[i] & ~0x1F) | 6
        new_clip = (0, y1, new_w - SEAM - 1, y2)
    elif new_w >= 512:
        new_clip = (-1, y1, 0x7FFFFFFF, y2)
    else:
        new_clip = (32, y1, new_w + 31, y2)

    with open(out, 'wb') as f:
        f.write(struct.pack('<5I', version, 0, lg_w, new_h, tilesets))
        f.write(struct.pack('<%dI' % len(out_cells), *out_cells))
        f.write(struct.pack('<4i', *new_clip))
        f.write(rest)
    print('wrote %s: %d x %d, %s, clip %s' % (out, new_w, new_h, mode, new_clip))


if __name__ == '__main__':
    main()
