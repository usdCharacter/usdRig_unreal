"""Prints World 1-1 of the RigExec platformer (ARigExecPlatformerLevel) as
C++ string rows, for the Layout default in RigExecPlatformerLevel.cpp.
Grid: H rows, row 0 is the ground; x is the column (one tile = 1 m)."""
W, H = 150, 13
g = [[" "] * W for _ in range(H)]


def put(x, y, c):
    g[y][x] = c


def ground(x0, x1):
    for x in range(x0, x1):
        put(x, 0, "#")


def row(x0, y, text):
    for i, c in enumerate(text):
        if c != ".":
            put(x0 + i, y, c)


def pipe(x, h):
    for y in range(1, h):
        put(x, y, "P"); put(x + 1, y, "P")
    put(x, h, "T"); put(x + 1, h, "T")


def coins(x0, y, n, step=1):
    for i in range(n):
        put(x0 + i * step, y, "o")


def arc(x0, y0, n, h):
    # Coins along a jump's arc.
    for i in range(n):
        t = i / (n - 1)
        put(x0 + i, y0 + round(h * 4 * t * (1 - t)), "o")


# Ground, with three gaps.
ground(0, 44); ground(47, 62); ground(65, 98); ground(101, W)
put(3, 1, "S")

# Opening: a ? block, then a row of bricks and ?s with one up high.
row(10, 4, "?")
row(15, 4, "B?B?B")
row(17, 8, "?")
coins(15, 6, 5)

# Pipes, rising.
pipe(24, 2)
pipe(32, 3)
arc(27, 4, 4, 2)

# First gap, coins over it.
arc(43, 4, 5, 3)

# A floating brick shelf with coins on it, and ?s above.
row(50, 4, "BBBBBB")
coins(50, 5, 6)
row(52, 8, "??")

# Second gap.
arc(61, 4, 5, 3)

# Mario's twin staircase across the third gap.
for i in range(4):
    for y in range(1, i + 2):
        put(66 + i, y, "X")
for i in range(4):
    for y in range(1, 4 - i + 1):
        put(72 + i, y, "X")
coins(68, 6, 6)

# ?s and bricks, a pipe, then the last gap.
row(80, 4, "?B?B?")
row(82, 8, "B?B")
pipe(88, 2)
arc(96, 3, 6, 3)

# The final staircase, up eight, then the flag and the castle.
for i in range(8):
    for y in range(1, i + 2):
        put(105 + i, y, "X")
put(122, 1, "F")
row(127, 1, "CCCCC")
row(127, 2, "CCDCC")
row(127, 3, "CCCCC")
row(128, 4, "CCC")

rows = ["".join(r) for r in reversed(g)]
for r in rows:
    print('\t\tTEXT("%s"),' % r)
