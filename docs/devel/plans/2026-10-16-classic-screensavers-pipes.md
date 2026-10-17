# Classic screensavers: faithful 3D Pipes

**Goal:** Rebuild the 3D Pipes saver so it looks like the Windows NT original: fat shaded
cylinders, mixed elbow and ball joints, the 16 SGI materials under one fixed light, the
occasional teapot, 2-4 pipes at once, a screen that fills, then a blocky 2 s dissolve and a new
screen turned 9.73156 degrees round. A near piece always covers a far one.

**Architecture:** A pure `PipesSim` (no LVGL) in `include/screensaver_pipes_sim.h` /
`src/ui/screensaver_pipes_sim.cpp`, built from five parts: the node grid and the growth rules,
`PipesScreen` (one screen's pipes, emitting `PipePiece`s, no pixels), mesh builders,
`PipesRenderer` (a small triangle rasterizer with a float inverse-depth z-buffer and per-material
lighting tables, writing through `PixelWriter`), and `PipesDissolve`. `PipesScreensaver` becomes a
thin `SaverBase` wrapper like `MazeScreensaver`, drawing at `SAVER_BUILD_CANVAS_FORMAT`. All
`lv_draw_*` code goes.

*Builder's choices settled here (the spec leaves them open):* direct `PixelWriter` rendering, not
`lv_draw_*`; a z-buffer over painter's order (pieces cross depth bands, and a z-buffer makes draw
order irrelevant by construction); triangle meshes rasterized with per-pixel normals, so the
highlight is a function of the eye-space normal; per-material lighting tables instead of a pow
per pixel; a three-rung ladder (below).

**Tech stack:** C++17, Catch2 (`tests/unit/`), `PixelWriter` / `FrameTarget`
(`include/screensaver_pixel_writer.h`), `StepAccumulator` (`include/screensaver_motion.h`).

**Spec:** `docs/devel/plans/2026-10-16-classic-screensavers-design.md` § "1. 3D Pipes".
**Original source** (the numbers below were read from it): the NT OpenGL SDK sample,
`samples/WIN32/OPENGL/SCRSAVE/{PIPES,COMMON}` in github.com/DigitalMars/dmc
(`VIEW.CXX`, `STATE.CXX`, `NSTATE.CXX`, `NPIPE.CXX`, `NODE.CXX`, `NODE.H`, `COMMON/MATERIAL.C`,
`COMMON/CLEAR.CXX`). Read it for behaviour; translate nothing line for line.

**Verified header:** a candidate of the full new header, syntax-checked against the repo's flags,
is at `/tmp/claude-1000/-home-pbrown-Code-Printing-helixscreen/41e9fa45-f64c-4947-864e-448345f897fc/scratchpad/plan_headers/pipes/screensaver_pipes_sim.h`
(and `screensaver_pipes.h` beside it). The interfaces in each task below are taken from it. If the
scratchpad is gone, the task blocks are the contract.

## Global Constraints

From the spec and the original's source. Every value is a named `constexpr` in
`screensaver_pipes_sim.h`.

| Name | Value | Source |
|---|---|---|
| `PIPES_NODES_LONG` | 15 nodes on the long screen axis; short axis `int(15 / aspect)` (at least 1); depth = 15. 800x480 = 15 x 9 x 15, 480x800 = 9 x 15 x 15 | `VIEW.CXX` `CalcNodeArraySize`, `NUM_DIV 16` |
| `PIPES_PITCH` | 7.0 (node spacing) | `divSize` |
| `PIPES_RADIUS` | 1.0 = pitch / 7 | `STATE.CXX radius` |
| ball (joint, start and end cap) | radius `sqrt(2) r / cos(pi / PIPES_SLICES)` | `NSTATE.CXX bigBall` |
| elbow | quarter torus, bend radius = tube radius = r, from r behind the node to r ahead | `OBJECTS.CXX`, `NPIPE.CXX DrawJoint` |
| straight step | long cylinder, length 7, from r behind the node | `NPIPE.CXX Draw` |
| after a joint or start cap | short cylinder, length 7 - 2r = 5, from r ahead of the node | `shortPipe` |
| teapot | Newell teapot at the node, scale 0.5 x 2.5 r per data unit | `auxSolidTeapot(2.5 * radius)` |
| `PIPES_VIEW_DIST` | 75 (camera at z = +75 looking down -z) | `zTrans = -75` |
| FOV | 90 deg vertical, square pixels: focal length = internal height / 2 | `persp.viewAngle` |
| `PIPES_YROT_STEP_DEG` | 9.73156 per new screen, starting at 0, mod 360 | `IncrementSceneRotation` |
| `PIPES_MIN_ACTIVE` / `PIPES_MAX_ACTIVE` | 2 / 4: active pipes = `min(7, rand[2, 4])`, inclusive | `ss_iRand2(2, MAX_DRAW_THREADS)` |
| `PIPES_PER_SCREEN` | 7 = `int(NORMAL_PIPE_COUNT 5 * 1.5)` | `STATE.CXX FrameReset` |
| `PIPES_MAX_WEIGHT_STRAIGHT` | 100 | `NODE.H` |
| straight weight | `1 + rand(4)` (1..4); 1 time in 20, `rand[25, 100]` inclusive | `NPIPE.CXX` |
| joints | teapot 1 in 1000 (`PIPES_TEAPOT_ODDS`), else ball 1 in 3, else elbow | `NSTATE.CXX ChooseJointType` |
| `PIPES_TURN_LOOKAHEAD` | 2 | spec; `GetBestPossibleTurns searchRadius` |
| light | directional, eye space, `normalize(90, 90, 150)` = (0.4575, 0.4575, 0.7625); infinite viewer, H = `normalize(L + (0,0,1))` = (0.2437, 0.2437, 0.9387) | `STATE.CXX GLInit` |
| lighting | `1.1 ka + kd max(0, n.L) + ks max(0, n.H)^shininess` (specular only where n.L > 0), clamped to 1 per channel. 1.1 = light-model ambient 1.0 + light ambient 0.1; light diffuse and specular 1.0 | `GLInit`, GL 1.x fixed function |
| shininess | `specExp * 128` | `ss_SetMaterial` |
| `PIPES_DISSOLVE_MS` | 2000 of frame time | `CLEAR.CXX idealTime = 2.0f` |
| dissolve block | `max(1, min(w, h) / 32)` px (15 at 480), every block once, random order | `SS_CLEAR_BASE_DIV 32` |
| `PIPES_STEP_MS` / `PIPES_MAX_STEPS_PER_TICK` | 100 / 3 (unchanged from today) | existing saver |
| `PIPES_SLICES` | 16, a multiple of 4 so cylinder and elbow end rings share vertices | builder |

**The 16 materials**, `COMMON/MATERIAL.C` `goodMaterials` order. `shininess` in the table is the
file's `specExp`; the code stores `specExp * 128`.

| # | name | ambient | diffuse | specular | specExp |
|---|---|---|---|---|---|
| 0 | emerald | 0.0215, 0.1745, 0.0215 | 0.07568, 0.61424, 0.07568 | 0.633, 0.727811, 0.633 | 0.6 |
| 1 | jade | 0.135, 0.2225, 0.1575 | 0.54, 0.89, 0.63 | 0.316228 x3 | 0.1 |
| 2 | pearl | 0.25, 0.20725, 0.20725 | 1.0, 0.829, 0.829 | 0.296648 x3 | 0.088 |
| 3 | ruby | 0.1745, 0.01175, 0.01175 | 0.61424, 0.04136, 0.04136 | 0.727811, 0.626959, 0.626959 | 0.6 |
| 4 | turquoise | 0.1, 0.18725, 0.1745 | 0.396, 0.74151, 0.69102 | 0.297254, 0.30829, 0.306678 | 0.1 |
| 5 | brass | 0.329412, 0.223529, 0.027451 | 0.780392, 0.568627, 0.113725 | 0.992157, 0.941176, 0.807843 | 0.21794872 |
| 6 | bronze | 0.2125, 0.1275, 0.054 | 0.714, 0.4284, 0.18144 | 0.393548, 0.271906, 0.166721 | 0.2 |
| 7 | copper | 0.19125, 0.0735, 0.0225 | 0.7038, 0.27048, 0.0828 | 0.256777, 0.137622, 0.086014 | 0.1 |
| 8 | gold | 0.24725, 0.1995, 0.0745 | 0.75164, 0.60648, 0.22648 | 0.628281, 0.555802, 0.366065 | 0.4 |
| 9 | silver | 0.19225 x3 | 0.50754 x3 | 0.508273 x3 | 0.4 |
| 10 | cyan plastic | 0.0, 0.1, 0.06 | 0.0, 0.50980392, 0.50980392 | 0.50196078 x3 | 0.25 |
| 11 | white plastic | 0, 0, 0 | 0.55 x3 | 0.70 x3 | 0.25 |
| 12 | yellow plastic | 0, 0, 0 | 0.5, 0.5, 0.0 | 0.60, 0.60, 0.50 | 0.25 |
| 13 | cyan rubber | 0.0, 0.05, 0.05 | 0.4, 0.5, 0.5 | 0.04, 0.7, 0.7 | 0.078125 |
| 14 | green rubber | 0.0, 0.05, 0.0 | 0.4, 0.5, 0.4 | 0.04, 0.7, 0.04 | 0.078125 |
| 15 | white rubber | 0.05 x3 | 0.5 x3 | 0.7 x3 | 0.078125 |

**Ladder** (*builder's call*): `PIPES_LEVELS = {{16 ms, scale 1}, {33 ms, scale 1}, {50 ms, scale 2}}`.
Between growth steps a frame draws nothing, so cost is one step's pieces every 100 ms plus the
dissolve; the bottom rung quarters the raster work and drops the refresh rate. A level request
changes the period at once; the scale applies at the next screen (the natural break), because
the z-buffer is sized for the scale.

**Framework rules** (`docs/devel/SCREENSAVERS.md` § "Adding a saver"): allocate in `on_start`,
free with capacity in `on_stop` (swap with empty), no static buffers (small `constexpr` tables in
`.rodata` are fine: the material table, the teapot control points), motion on frame time only,
both depths through `PixelWriter`.

## Spec divergences found against the source (settled here, reported to the controller)

- The spec says "16 nodes along the long axis"; the source has 16 *divisions* and 15 nodes, which
  is what the spec's own "about 15x9x15" means. The plan uses 15.
- The spec's "a turn picks the direction with the most empty nodes, looking up to 2 ahead" is the
  original's *flex*-pipe rule (`GetBestPossibleTurns`); its normal pipes pick a uniformly random
  empty neighbour (`ChooseRandomDirection`). The plan follows the spec but keeps the original's
  straight-versus-turn odds: roll `rand(weight + empty turns)` exactly as the original, and only
  when the roll lands on a turn, pick among the turns with the longest empty run (up to 2).
- A pipe whose start node has no empty neighbour draws a teapot there and is stuck at once
  (`NPIPE.CXX Start`). Kept: it is the original's other teapot.

## Review Focus

1. **Draw order leaking into the picture.** A missing or inverted depth test, or depth
   interpolated affinely instead of as 1/z, lets a later far piece paint over a near one.
   Test: Task 5 "draw order never changes the picture".
2. **A screen-space highlight.** Shading from screen position or a fixed stripe instead of the
   eye-space normal. Test: Task 5 "the highlight sits up-right of centre on every ball".
3. **The fill/reset count off by one.** Killing at `>= 7`, counting only replacements, or never
   capping when nodes run out changes how full a screen gets. Tests: Task 2 "a screen starts
   exactly 7 pipes" and "a grid that runs out of nodes ends the screen".
4. **Dissolve paced by frames, or leftover growth leaking into it.** Test: Task 6 "the dissolve
   clears by frame time" and Task 7 "a finished screen dissolves for 2 s, then turns".
5. **A scale change mid-screen.** Resizing the z-buffer under live pieces writes out of rows or
   mismatches depth. Tests: Task 7 "a level's scale waits for the next screen" and Task 5
   "frames of every size and scale stay inside their rows".

---

## Task 1: Grid and growth rules

**Files**
- Create: `include/screensaver_pipes_sim.h` (constants, enums, materials table, `PipesGrid`, the choosers)
- Create: `src/ui/screensaver_pipes_sim.cpp`
- Create: `tests/unit/test_screensaver_pipes_sim.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt` (add `src/ui/screensaver_pipes_sim.cpp  # not in the v1 Core+AMS cut`, sorted)

**Interfaces** (namespace `helix::ui`, whole file under `#ifdef HELIX_ENABLE_SCREENSAVER`)
- Consumes: `screensaver::random_below` (`include/screensaver_motion.h`).
- Produces:
```cpp
enum class PipeDir : uint8_t { POS_X, NEG_X, POS_Y, NEG_Y, POS_Z, NEG_Z, NONE };
enum class PipeJoint : uint8_t { ELBOW, BALL, TEAPOT };
struct PipeNode { int x = 0, y = 0, z = 0; bool operator==(const PipeNode&) const; };
struct PipeGridDims { int x = 0, y = 0, z = 0; };
PipeGridDims pipes_grid_dims(int32_t w, int32_t h);
PipeNode pipe_step(PipeNode n, PipeDir d);
PipeDir pipe_opposite(PipeDir d);
class PipesGrid {
  public:
    void reset(PipeGridDims dims);   // all empty
    void release();                  // capacity 0
    PipeGridDims dims() const;
    bool in_bounds(PipeNode n) const;
    bool empty(PipeNode n) const;    // in bounds and not taken
    void take(PipeNode n);
    size_t empty_count() const;
    int empty_run(PipeNode from, PipeDir d, int limit) const;      // `from` excluded
    bool random_empty(std::minstd_rand& rng, PipeNode& out) const; // 2 x node-count random tries, then a scan
    size_t buffer_bytes() const;
};
int pipes_choose_weight_straight(std::minstd_rand& rng);
PipeJoint pipes_choose_joint(std::minstd_rand& rng);
PipeDir pipes_choose_direction(const PipesGrid& grid, PipeNode pos, PipeDir last,
                               int weight_straight, std::minstd_rand& rng);
```
`pipes_choose_direction`: if `weight == 100` and straight is empty, straight. Else let
`w = straight empty ? weight : 0`, `T` = empty neighbours other than straight (the reverse
counts when empty, as at a start); `NONE` if `w + |T| == 0`; roll `random_below(w + |T|)`; below
`w` goes straight, otherwise pick uniformly among the members of `T` whose `empty_run(pos, d, 2)`
is largest. `pipes_choose_joint`: `random_below(1000) == 153` is a teapot (one draw), else
`random_below(3) == 0` is a ball. Weight: `random_below(20) == 0 ? 25 + random_below(76) : 1 + random_below(4)`.

- [ ] **Write failing tests** (tag `[screensaver][pipes_sim]`):
```cpp
TEST_CASE("the grid follows the screen's aspect with 15 nodes on the long axis", ...) {
    CHECK(dims_eq(pipes_grid_dims(800, 480), {15, 9, 15}));
    CHECK(dims_eq(pipes_grid_dims(480, 800), {9, 15, 15}));
    CHECK(dims_eq(pipes_grid_dims(480, 480), {15, 15, 15}));
    CHECK(dims_eq(pipes_grid_dims(1024, 600), {15, 8, 15}));
    CHECK(dims_eq(pipes_grid_dims(480, 272), {15, 8, 15}));
    CHECK(pipes_grid_dims(4000, 10).y == 1);
}
TEST_CASE("a grid counts what is taken and finds its last empty node", ...)
    // reset({3,2,2}) -> empty_count 12; take 11 nodes; random_empty finds the one left
    // (over 20 seeds, same node every time); take it -> random_empty false, empty_count 0;
    // in_bounds false for x=-1 and x=3; empty() false out of bounds.
TEST_CASE("empty_run stops at the first taken node and at the limit", ...)
    // 15x9x15, pos (7,4,7): run +X = 2 with limit 2; take (9,4,7) -> run +X = 1;
    // run -Y from (7,0,7) = 0 (edge).
TEST_CASE("a pipe with no empty neighbour goes nowhere", ...)
    // dims {1,1,1}: NONE for every `last` and weight 1..100.
TEST_CASE("the heaviest straight weight always goes straight while it can", ...)
    // empty 15x9x15, pos (7,4,7), last POS_X, weight 100: 1000 draws all POS_X.
    // take (8,4,7): 1000 draws, none POS_X, none NONE.
TEST_CASE("straight carries its weight against the empty turns", ...)
    // pos (7,4,7), last POS_X, reverse (6,4,7) taken, weight 4: 6000 draws,
    // straight fraction in [0.47, 0.53] (4 / (4 + 4)).
    // reverse left empty: fraction in [0.41, 0.48] (4 / 9).
TEST_CASE("a turn goes where the most empty nodes lie, two ahead", ...)
    // pos (7,4,7), last POS_X, take (8,4,7) and (6,4,7); take (7,6,7) -> +Y run 1;
    // take (7,4,9), (7,4,5) -> +Z, -Z runs 1; -Y run 2.  500 draws: every one NEG_Y.
    // free (7,6,7): +Y run 2 (two empty, then the edge-free rest) ties -Y, whose 4 empty nodes
    // count as 2 (the lookahead caps there) -> both POS_Y and NEG_Y occur, nothing else.
    // free (7,4,9), take (7,4,10): +Z run 2 joins the tie -> POS_Y, NEG_Y, POS_Z all occur.
TEST_CASE("joints mix a third balls, the rest elbows, and a teapot 1 in 1000", ...) {
    std::minstd_rand rng(42);
    std::array<uint32_t, 3> n{};
    for (int i = 0; i < 1'000'000; i++) n[static_cast<size_t>(pipes_choose_joint(rng))]++;
    CHECK(n[2] >= 850); CHECK(n[2] <= 1150);
    const double ball = double(n[1]) / (n[0] + n[1]);
    CHECK(ball > 0.330); CHECK(ball < 0.337);
}
TEST_CASE("a pipe's straight weight is 1-4, or 25-100 one time in 20", ...)
    // 100000 draws: every value in [1,4] or [25,100]; fraction >= 25 in [0.045, 0.055];
    // each of 1..4 in [0.23, 0.245]; 25 and 100 both seen.
```
- [ ] Run `make t F='[pipes_sim]'`: expected FAIL (no such header).
- [ ] Implement the interfaces above in `src/ui/screensaver_pipes_sim.cpp`. The grid stores one
  byte per node, x fastest. `random_empty` tries `2 * node_count` random nodes, then scans x, y, z.
- [ ] Run `make t F='[pipes_sim]'`: expected PASS.
- [ ] Commit: `git add -N include/screensaver_pipes_sim.h src/ui/screensaver_pipes_sim.cpp tests/unit/test_screensaver_pipes_sim.cpp && git commit -m "feat(screensaver): pipes grid and growth rules from the NT original" -- include/screensaver_pipes_sim.h src/ui/screensaver_pipes_sim.cpp tests/unit/test_screensaver_pipes_sim.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`

## Task 2: One screen of pipes (`PipesScreen`), no pixels

**Files**
- Modify: `include/screensaver_pipes_sim.h`, `src/ui/screensaver_pipes_sim.cpp`
- Test: `tests/unit/test_screensaver_pipes_sim.cpp`

**Interfaces**
- Consumes: Task 1.
- Produces:
```cpp
struct PipeVec3 { float x = 0, y = 0, z = 0; };
enum class PipePieceKind : uint8_t { CYLINDER, ELBOW, BALL, TEAPOT };
struct PipePiece {
    PipePieceKind kind = PipePieceKind::CYLINDER;
    PipeVec3 at;        // CYLINDER: axis start; otherwise the joint's node centre
    PipeDir dir = PipeDir::POS_X;   // CYLINDER axis; ELBOW heading out
    PipeDir from = PipeDir::POS_X;  // ELBOW heading in
    float length = 0.0f;            // CYLINDER only
    uint8_t material = 0;
};
struct PipeState {
    PipeNode pos; PipeDir last = PipeDir::POS_X; int weight_straight = 1; uint8_t material = 0;
    int segment_count = 0; bool alive = false; bool stuck = false;
    bool operator==(const PipeState&) const;
};
PipeVec3 pipes_node_world(PipeNode n, PipeGridDims dims); // (i - (dim-1)/2) * 7 per axis
PipeVec3 pipes_dir_vec(PipeDir d);
void pipes_emit_start(PipeNode node, PipeDir first, PipeGridDims dims, uint8_t material,
                      std::vector<PipePiece>& out);
void pipes_emit_move(PipeNode node, PipeDir last, PipeDir next, PipeJoint joint, PipeGridDims dims,
                     uint8_t material, std::vector<PipePiece>& out);
void pipes_emit_end(PipeNode node, PipeGridDims dims, uint8_t material, std::vector<PipePiece>& out);
class PipesScreen {
  public:
    void begin(PipeGridDims dims, std::minstd_rand& rng, std::vector<PipePiece>& out);
    bool step(std::minstd_rand& rng, std::vector<PipePiece>& out); // false once finished
    void release();
    const PipesGrid& grid() const;
    PipesGrid& grid();                         // tests build crowded grids by hand
    const std::array<PipeState, PIPES_MAX_ACTIVE>& pipes() const;
    int pipes_counted() const;                 // toward PIPES_PER_SCREEN, starting pipes included
    int pipes_started() const;
    bool finished() const;                     // no slot alive
    const std::array<uint32_t, 3>& joint_counts() const; // turn joints by PipeJoint since begin()
};
```
**Rules** (`STATE.CXX Draw`/`FrameReset`, `NPIPE.CXX`):
- `begin`: `grid.reset(dims)`, zero the counts, `n = min(PIPES_PER_SCREEN, 2 + random_below(3))`,
  `counted = n`, start slots `0..n-1`. A start that finds no empty node kills its slot and sets
  `max_pipes = counted`.
- Start: `random_empty` → take it; material `random_below(16)`; weight from Task 1; `last` =
  `random_below(6)`; `first = pipes_choose_direction(...)`. `NONE`: emit a TEAPOT at the node,
  `stuck = true`. Else `pipes_emit_start`, take `pipe_step(node, first)`, move there, `last = first`.
- `step`: (1) for each live stuck slot: `if (++counted > max_pipes)` kill it; else start it, and a
  failed start kills it and sets `max_pipes = counted`. (2) For each live, unstuck pipe:
  `next = choose_direction`; `NONE` → `pipes_emit_end`, `stuck = true`; else
  `joint = next != last ? pipes_choose_joint(rng) : (none)`, count it in `joint_counts`,
  `pipes_emit_move`, take the next node, move, `last = next`, `segment_count++`.
  Returns `!finished()`.
- Emission geometry: start = BALL at the node, then CYLINDER at `node + r * first`, length 5.
  Straight move = CYLINDER at `node - r * next`, length 7. Turn = ELBOW (`from = last`, `dir =
  next`) or BALL or TEAPOT at the node, then CYLINDER at `node + r * next`, length 5. End = BALL.

- [ ] **Write failing tests**:
```cpp
TEST_CASE("pieces sit where the original draws them", ...)
    // dims {15,9,15}; node (7,4,7) is world (0,0,0); node (0,0,0) is (-49,-28,-49).
    // emit_start((7,4,7), POS_Y): [BALL at (0,0,0)], [CYLINDER at (0,1,0), dir POS_Y, length 5].
    // emit_move((7,4,7), POS_X, POS_X, any): [CYLINDER at (-1,0,0), POS_X, 7].
    // emit_move(.., POS_X, POS_Z, ELBOW): [ELBOW at (0,0,0) from POS_X dir POS_Z],
    //   [CYLINDER at (0,0,1), POS_Z, 5]. BALL and TEAPOT likewise with their kind.
    // emit_end: [BALL at node]. Every piece carries the material passed in.
TEST_CASE("a screen begins with 2 to 4 pipes on distinct taken nodes", ...)
    // seeds 1..300: alive count in [2,4], == pipes_counted() == pipes_started(); counts 2, 3
    // and 4 all occur; positions distinct and !grid().empty(); material < 16.
TEST_CASE("a screen starts exactly 7 pipes before it finishes", ...)
    // seeds 1..40, dims {15,9,15}: step until false (bound 20000); pipes_started() == 7 and
    // grid().empty_count() > 0 for every seed.
TEST_CASE("a pipe stops only where every neighbour is taken", ...)
    // seed 5: after every step, each pipe with stuck == true has no empty neighbour
    // (6 pipe_step checks with grid().empty()). REQUIRE some pipe got stuck.
TEST_CASE("a grid that runs out of nodes ends the screen", ...)
    // dims {2,1,1}: begin places at most 2 pipes; both are TEAPOTs (no empty neighbour);
    // step() returns false within 2 calls; pipes_started() <= 2.
    // dims {1,1,1}: begin emits exactly one TEAPOT; first step() returns false.
TEST_CASE("over many turns a third of the joints are balls", ...)
    // seed 99, dims {15,9,15}: run whole screens, summing joint_counts(), until
    // elbow + ball + teapot >= 20000. ball / (ball + elbow) in [0.31, 0.355];
    // teapot <= 0.004 * total. Also: segment steps that went straight > turns (straight is
    // weighted), sum over the same run.
TEST_CASE("a screen replays exactly from a seed", ...)
    // two screens, same seed, 300 steps: identical pipes() after every step and identical
    // piece vectors.
```
- [ ] Run `make t F='[pipes_sim]'`: expected FAIL.
- [ ] Implement the rules above.
- [ ] Run `make t F='[pipes_sim]'`: expected PASS.
- [ ] Commit: `git commit -m "feat(screensaver): pipes screen: 7 pipes per screen, mixed joints, caps" -- include/screensaver_pipes_sim.h src/ui/screensaver_pipes_sim.cpp tests/unit/test_screensaver_pipes_sim.cpp`

## Task 3: Materials and lighting

**Files:** as Task 2.

**Interfaces**
```cpp
struct PipeMaterial { float ka[3]; float kd[3]; float ks[3]; float shininess; }; // shininess = specExp*128
inline constexpr PipeMaterial PIPE_MATERIALS[] = { /* the 16 rows of the table above, in order */ };
inline constexpr size_t PIPE_MATERIAL_COUNT = std::size(PIPE_MATERIALS);
PipeVec3 pipes_light_dir();   // normalize(90, 90, 150)
PipeVec3 pipes_half_vector(); // normalize(L + (0, 0, 1))
Rgb pipes_shade(const PipeMaterial& m, PipeVec3 n);  // n unit; lround(clamp(c) * 255)
```
- [ ] **Write failing tests**:
```cpp
TEST_CASE("the material table is the original's 16", ...)
    // PIPE_MATERIAL_COUNT == 16; [0] emerald kd = {0.07568, 0.61424, 0.07568}, shininess 76.8;
    // [2] pearl kd {1.0, 0.829, 0.829}, shininess Approx(11.264); [5] brass shininess
    // Approx(27.897436); [15] white rubber ks {0.7,0.7,0.7}, shininess 10.0.
TEST_CASE("the light and half vector are fixed in eye space", ...)
    // L Approx (0.457496, 0.457496, 0.762493); H Approx (0.243674, 0.243674, 0.938747), eps 1e-5.
TEST_CASE("pipes_shade is GL fixed-function lighting", ...)
    // emerald at n = L: {27, 207, 27}; at n = H: {186, 255, 186}; at (0,0,-1): {6, 49, 6};
    // at (0,0,1): {22, 170, 22}. ruby at L: {207, 15, 15}; at H: {255, 173, 173}.
    // white rubber at (0,0,1): {206, 206, 206}; at (0,0,-1): {14, 14, 14}.
    // each channel within 1.
TEST_CASE("the brightest point of a material faces the half vector", ...)
    // for every material: shade(H) >= shade(n) channel-sum for 200 random unit n.
```
- [ ] Run: expected FAIL. Implement. Run: expected PASS.
- [ ] Commit: `git commit -m "feat(screensaver): the 16 SGI materials under the original's fixed light" -- include/screensaver_pipes_sim.h src/ui/screensaver_pipes_sim.cpp tests/unit/test_screensaver_pipes_sim.cpp`

## Task 4: Meshes, including the teapot

**Files**
- Modify: `include/screensaver_pipes_sim.h`, `src/ui/screensaver_pipes_sim.cpp`
- Create: `src/ui/screensaver_pipes_teapot.cpp` (the control points and `pipes_build_teapot`)
- Modify: `app_srcs_excluded.txt` (add `src/ui/screensaver_pipes_teapot.cpp`)
- Test: `tests/unit/test_screensaver_pipes_sim.cpp`

**Interfaces**
```cpp
struct PipeVertex { PipeVec3 p; PipeVec3 n; };            // n unit, outward
struct PipeMesh { std::vector<PipeVertex> verts; std::vector<uint16_t> tris; size_t buffer_bytes() const; };
void pipes_build_cylinder(PipeMesh& out); // +z from 0 to 1, radius r, PIPES_SLICES facets, open ends
void pipes_build_ball(PipeMesh& out);     // radius sqrt(2) r / cos(pi / 16), 16 slices x 8 stacks
void pipes_build_elbow(PipeMesh& out);    // enters (0,0,-r) heading +z, leaves (0,r,0) heading +y,
                                          // bend centre (0,r,-r); 16 slices x 6 bend segments
void pipes_build_teapot(PipeMesh& out);
struct PipeXform { PipeVec3 x, y, z, t; };  // p_world = p.x*x + p.y*y + p.z*z + t
PipeXform pipes_piece_xform(const PipePiece& piece);
```
- Ring vertices of the cylinder and both elbow end rings sit at angles `k * 2 pi / 16` in an
  axis-aligned basis of their plane. The cylinder's basis for each axis is fixed (`pipes_piece_xform`:
  POS_X/NEG_X use (y, z), POS_Y/NEG_Y use (z, x), POS_Z/NEG_Z use (x, y), signs chosen so x cross y = dir).
  Because 16 is a multiple of 4, the end rings of an elbow and the cylinders meeting it share
  vertex positions, so there is no crack.
- All triangles wind counter-clockwise seen from outside (the renderer culls back faces, as the
  original's `glCullFace(GL_BACK)` does).
- **Teapot:** take `patchdata_teapot` (10 input patches) and `cpdata_teapot` from freeglut's
  `src/fg_teapot_data.h` (github.com/freeglut/freeglut; X11/MIT licence, compatible with
  GPL-3.0-or-later). Keep freeglut's permission notice in a comment above the data, with the
  SPDX header for the file. Expand as freeglut's comment says: rim, body, lid and bottom (patches
  0-5) into all four quadrants (x, y sign pairs (1,1), (-1,1), (1,-1), (-1,-1)); handle and spout
  (6-9) mirrored across y = 0 (in this data the half-handle and half-spout lie at y <= 0). A
  mirror with an odd number of sign flips reverses winding: swap two indices of each of its
  triangles and keep normals outward. Tessellate each bicubic patch at 6 x 6 vertices; normals
  from the cross product of the partial derivatives, falling back to the neighbouring sample's at
  degenerate corners (the lid top, the bottom centre). Transform the data the way
  `auxSolidTeapot` does: `(x, y, z) -> s * (x, z - 1.5, -y)` with `s = 0.5 * 2.5 * PIPES_RADIUS`.
  The whole mesh is built in `on_start` into a vector (control points are `constexpr`, in `.rodata`).

- [ ] **Write failing tests**:
```cpp
TEST_CASE("every mesh has unit outward normals and outward winding", ...)
    // cylinder, ball, elbow: every |n| within 1e-4 of 1; n . (p - axis/centre point) > 0;
    // for every triangle cross(b-a, c-a) . (na+nb+nc) > 0.
    // teapot: |n| == 1 within 1e-3; winding test holds for >= 98% of non-degenerate triangles.
TEST_CASE("elbow end rings meet the cylinders without a crack", ...)
    // For each (from, dir) pair of perpendicular directions: transform the elbow's start ring
    // and the ring at length 1 of a cylinder along `from` ending at node - r*from; the two sets
    // of 16 world positions match within 1e-4 (as sets). Same for the elbow's end ring and a
    // cylinder along `dir` starting at node + r*dir.
TEST_CASE("the ball is the original's big ball", ...)
    // every ball vertex lies at sqrt(2) r / cos(pi/16) = 1.441901 from the centre (1e-5): a pipe
    // end ring r from the centre sits at sqrt(2) r, inside the faceted surface.
TEST_CASE("the teapot is the original's size", ...)
    // bounding box of pipes_build_teapot: x in [-3.75, 4.40625], y in [-1.875, 2.0625] (lid
    // knob top), z in [-2.5, 2.5] (body radius 2.0 * 1.25), each within 0.02.
TEST_CASE("an elbow's centreline bends round (0, r, -r)", ...)
    // the mean of each ring's vertices lies at distance r from (0, r, -r) in the y-z plane.
```
- [ ] Run: expected FAIL. Implement. Run: expected PASS.
- [ ] Commit: `git add -N src/ui/screensaver_pipes_teapot.cpp && git commit -m "feat(screensaver): pipe, elbow, ball and teapot meshes" -- include/screensaver_pipes_sim.h src/ui/screensaver_pipes_sim.cpp src/ui/screensaver_pipes_teapot.cpp tests/unit/test_screensaver_pipes_sim.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`

## Task 5: The z-buffered renderer

**Files:** `include/screensaver_pipes_sim.h`, `src/ui/screensaver_pipes_sim.cpp`, tests as before.

**Interfaces**
- Consumes: Tasks 2-4, `PixelWriter::put_dithered`, `DirtyRect`.
- Produces:
```cpp
class PipesRenderer {
  public:
    void init(uint32_t w, uint32_t h, uint8_t scale); // canvas size; builds meshes and lighting tables
    void release();                                     // every vector swapped empty
    void set_view(float yrot_deg, PipeGridDims dims);
    void clear_depth();
    DirtyRect draw(const FrameTarget& frame, const PipePiece& piece); // canvas box written, maybe empty
    bool project(PipeVec3 p, float& sx, float& sy) const;            // canvas pixels
    uint8_t scale() const;
    size_t buffer_bytes() const;
};
```
**Pipeline** (pin these; they are what the tests check):
- World to eye: `Ry(yrot)` with GL's sense (`x' = x cos + z sin`, `z' = -x sin + z cos`), then
  `z_e = z' - 75`. Depth `d = -z_e`. Internal size `iw = ceil(w / scale)`, `ih = ceil(h / scale)`,
  focal `f = ih / 2`. `sx = iw / 2 + f * x_e / d`, `sy = ih / 2 - f * y_e / d`; `project()` returns
  those times `scale`. Normals rotate with `Ry` only.
- Skip a triangle with any vertex at `d < 0.5` (cannot happen inside the grid; guards the teapot).
  Cull triangles with non-positive screen-space area (counter-clockwise front faces in screen
  coordinates with y down: check the sign once in a test).
- Rasterize over the clamped bounding box, sampling pixel centres (+0.5), top-left fill rule.
  Interpolate `1/d` and the eye normal linearly in screen space; renormalise the normal per pixel.
  Depth test: write only where `1/d` > the buffer's (float, `iw * ih`, cleared to 0).
- Shade: per material, an ambient-plus-diffuse table of 256 entries over `n.L` in [0, 1] and a
  specular table of 1024 over `n.H` in [0, 1], stored as `uint16_t` channel values times 255
  before clamping; pixel = `min(255, ad[lround(nl*255)] + (nl > 0 ? sp[lround(nh*1023)] : 0))`;
  `n.L <= 0` reads `ad[0]` (= 1.1 ka). Written with `put_dithered`, as a `scale x scale` block
  clipped to the frame.
- The dirty box is the projected bounding box of the piece's transformed vertices, padded by one
  internal pixel, scaled, clamped to the frame.

- [ ] **Write failing tests** (frames are heap `Frame` helpers as in `test_screensaver_maze.cpp`,
  rows padded by 12 bytes):
```cpp
TEST_CASE("the camera projects the original's view", ...)
    // init(800,480,1), set_view(0, {15,9,15}): (0,0,0) -> (400,240); (0,0,50) -> (400,240);
    // (7,0,0) -> (422.4, 240) within 0.01; (0,7,0) -> (400, 217.6).
    // set_view(90, ..): (10,0,0) -> (400,240) and its depth is 85 (project (10,7,0): sy = 240 - 240*7/85).
TEST_CASE("draw order never changes the picture", ...)
    // 800x480 XRGB8888. A: ruby BALL at (0,0,30); B: emerald BALL at (0.5,0,40). Frame 1 draws
    // A then B, frame 2 draws B then A (fresh clear_depth + black frames). Hashes equal; pixel
    // (400,240) is emerald-dominant (g > 2 r). Repeat with a CYLINDER crossing behind a BALL.
TEST_CASE("a pipe is as fat as its radius over its depth", ...)
    // CYLINDER along POS_X at (-3.5,0,50), length 7, yrot 0: column x = 400 lit rows in
    // [18, 21] (2 * r * 240 / 25 = 19.2). At (-3.5,0,-25), depth 100: [4, 6].
TEST_CASE("the highlight sits up-right of centre on every ball", ...)
    // emerald BALL at (0,0,40): the brightest pixel (channel sum) is offset from
    // project(centre) by dx in [1,4], dy in [-4,-1] (the eye-space H lands 2.5 px up-right).
    // emerald BALL at (8,6,40): offset dx > 0, dy < 0 (perspective pushes it to about +4.7,
    // -4.2). The two brightest colours agree within 16 per channel and lie within 16 of
    // pipes_shade(emerald, H) = {186,255,186} (a pixel centre misses H by up to half a pixel).
    // A CYLINDER along POS_X: its brightest pixel lies above the axis's projected row; a
    // CYLINDER along POS_Y: its brightest pixel lies right of the axis's projected column.
TEST_CASE("an elbow joins its two pipes with no gap", ...)
    // long CYLINDER at node(6,4,7) - r X (length 7, POS_X), ELBOW at node (7,4,7) from POS_X
    // to POS_Y, short CYLINDER at node + r Y (length 5): every projected sample of the joined
    // centreline, every 0.05 units (the elbow part on its arc), is a lit pixel.
TEST_CASE("the renderer dirties every pixel a piece changes", ...)
    // 50 random pieces (seeded kinds, positions inside the grid, materials): changed pixels
    // outside the returned box == 0, and at least one piece changed pixels.
TEST_CASE("RGB565 and XRGB8888 renders of one scene agree to 565 precision", ...)
    // the same 30 pieces at 160x96: |dr| <= 8, |dg| <= 4, |db| <= 8 at every pixel.
TEST_CASE("an XRGB8888 pipes frame keeps every X byte opaque", ...)
TEST_CASE("frames of every size and scale stay inside their rows", ...)
    // sizes {1x1, 7x5, 161x97, 800x480} x scales {1, 2} x both formats: canary bytes in the
    // 12 padding bytes per row unchanged after drawing 40 pieces.
TEST_CASE("at scale 2 every 2x2 block is one colour", ...)
    // 160x96, scale 2: after drawing a BALL, every aligned 2x2 block has 4 equal pixels.
TEST_CASE("a released renderer holds no buffers", ...)
    // init(160,96,1): buffer_bytes() > 160*96*4; release(): buffer_bytes() == 0.
```
- [ ] Run: expected FAIL. Implement. Run: expected PASS.
- [ ] Commit: `git commit -m "feat(screensaver): z-buffered pipe renderer with per-material lighting tables" -- include/screensaver_pipes_sim.h src/ui/screensaver_pipes_sim.cpp tests/unit/test_screensaver_pipes_sim.cpp`

## Task 6: The digital dissolve

**Files:** as Task 5.

**Interfaces**
```cpp
class PipesDissolve {
  public:
    void init(uint32_t w, uint32_t h);   // block = max(1, min(w,h)/32); cols, rows by ceiling
    void release();
    void begin(std::minstd_rand& rng);   // Fisher-Yates order with random_below(i + 1), i = n-1..1
    void step(uint32_t dt_ms, const FrameTarget& frame, std::vector<DirtyRect>& dirty);
    bool active() const;
    size_t cleared() const;
    size_t block_count() const;
    int block_px() const;
    size_t buffer_bytes() const;
};
```
`step`: `elapsed += dt`; `due = min(n, n * elapsed / 2000)` in 64-bit; fill blocks
`[cleared, due)` black (clipped at the right and bottom edges), one `DirtyRect` each; inactive
once `cleared == n`.

- [ ] **Write failing tests**:
```cpp
TEST_CASE("the dissolve cuts the screen into 1/32 blocks", ...)
    // init(800,480): block_px 15, block_count 54*32 = 1728. init(480,272): 8, 60*34.
    // init(10,10): block 1 (10/32 = 0 -> 1), 100 blocks.
TEST_CASE("the dissolve clears by frame time", ...)
    // 800x480 frame filled white, begin(rng 3): 25 x step(40): cleared == 864;
    // to 1999 ms: cleared == 1727, active(); to 2000: cleared == 1728, !active(), every pixel 0.
    // A run of 50 x step(16) and a run of 16 x step(50) from the same seed end at 800 ms with
    // identical frames.
TEST_CASE("each dissolve block is cleared once and dirtied", ...)
    // over the whole run, the dirty rects equal the set of blocks (each once) and every
    // changed pixel lies inside that frame's rects.
TEST_CASE("two dissolves from one seed clear in the same order", ...)
```
- [ ] Run: expected FAIL. Implement. Run: expected PASS.
- [ ] Commit: `git commit -m "feat(screensaver): pipes digital dissolve over 2 s of frame time" -- include/screensaver_pipes_sim.h src/ui/screensaver_pipes_sim.cpp tests/unit/test_screensaver_pipes_sim.cpp`

## Task 7: `PipesSim`, screens in sequence

**Files:** as Task 5.

**Interfaces**
```cpp
struct PipesLevel { uint32_t period_ms; uint8_t scale; };
inline constexpr PipesLevel PIPES_LEVELS[] = {{SAVER_FAST_PERIOD, 1}, {33, 1}, {50, 2}};
inline constexpr size_t PIPES_LEVEL_COUNT = std::size(PIPES_LEVELS);
enum class PipesPhase : uint8_t { GROWING, DISSOLVING };
class PipesSim {
  public:
    void init(const FrameTarget& frame, std::minstd_rand& rng, size_t level); // fills black, yrot 0
    void step(uint32_t dt_ms, const FrameTarget& frame, std::minstd_rand& rng,
              std::vector<DirtyRect>& dirty);
    void set_level(size_t level);   // scale applies at the next screen
    void clear();                   // every buffer released
    PipesPhase phase() const;
    const PipesScreen& screen() const;
    const PipesRenderer& renderer() const;
    const PipesDissolve& dissolve() const;
    float yrot_deg() const;
    uint32_t screens_started() const;
    size_t buffer_bytes() const;
  private:
    screensaver::StepAccumulator steps_;   // reuse; do not hand-roll a carry
    // screen_, renderer_, dissolve_, phase_, yrot_deg_, level_, screens_, pieces_ (reused vector)
};
```
`step` while GROWING: `n = steps_.steps_due(dt, 100, 3)`; for each step, clear `pieces_`,
`screen_.step`, draw each piece and push its box; when the screen finishes, drop the remaining
steps, `dissolve_.begin(rng)`, phase DISSOLVING. While DISSOLVING: `dissolve_.step(dt, ...)`; when
it ends: `yrot = fmod(yrot + 9.73156, 360)`, re-`init` the renderer if the pending level's scale
differs, `clear_depth`, `set_view`, `screen_.begin` and draw its pieces, `steps_.reset()`, phase
GROWING, `screens_++`.

- [ ] **Write failing tests**:
```cpp
TEST_CASE("the first screen starts at yrot 0 with its starting pipes drawn", ...)
    // 160x96: after init, lit pixels > 0, phase GROWING, screens_started 1, yrot 0.
TEST_CASE("a finished screen dissolves for 2 s, then turns 9.73156 degrees", ...)
    // seed 8, 160x96, step(100) until phase DISSOLVING (bound 20000). Then 19 x step(100):
    // still DISSOLVING. One more: GROWING, yrot Approx(9.73156f), screens_started 2, every
    // segment_count 0. step(99): every segment_count 0. step(1): some segment_count == 1.
TEST_CASE("growth steps still due when a screen finishes are dropped", ...)
    // sim A, seed 8: count the step(100) calls k until DISSOLVING. Sim B, same seed: k - 1
    // step(100) calls, then step(300): phase DISSOLVING and dissolve().cleared() == 0. Then
    // step(1000): cleared == block_count() / 2 exactly (the 300 ms did not carry over).
TEST_CASE("pipes grow one step per 100 ms, at most three per call", ...)
    // step(300) == three step(100)s (same pipes(), same frame hash); step(450) grows three,
    // a following step(0) grows none, step(50) grows one.
TEST_CASE("a level's scale waits for the next screen", ...)
    // init level 0 at 160x96; set_level(2): renderer().scale() == 1 until the next screen
    // starts, then 2. set_level(0) mid-screen at scale 2: stays 2 until the next.
TEST_CASE("PipesSim dirties every pixel its step changes", ...)
    // 160x96, seed 12, step(50) x 3000 (covers growth, a dissolve and a second screen):
    // diff_frames(...).outside == 0 every frame.
TEST_CASE("a pipes run replays exactly from a seed", ...)
    // two sims, seed 21, 1500 x step(33): identical frame hashes every 100 frames.
TEST_CASE("both depths run the same scene", ...)
    // RGB565 and XRGB8888 sims from one seed, 400 x step(100): same screen().pipes(), same
    // yrot, frames within 565 precision (8, 4, 8).
TEST_CASE("a cleared pipes sim holds no buffers", ...)
    // buffer_bytes() > 0 after init; 0 after clear(); screen().grid().buffer_bytes() == 0.
```
- [ ] Run: expected FAIL. Implement. Run: expected PASS.
- [ ] Commit: `git commit -m "feat(screensaver): PipesSim runs screens, dissolve and the turning view" -- include/screensaver_pipes_sim.h src/ui/screensaver_pipes_sim.cpp tests/unit/test_screensaver_pipes_sim.cpp`

## Task 8: Wire the saver; replace the old drawing and its tests

**Files**
- Modify: `include/screensaver_pipes.h`, `src/ui/screensaver_pipes.cpp` (rewrite)
- Modify: `include/screensaver.h` (rename `PIPES_CANVAS_FORMAT` to `LV_DRAW_CANVAS_FORMAT`, its doc
  now "a canvas drawn with lv_draw_*"), `include/screensaver_flip_clock.h` (uses the new name;
  comment no longer says "the pipes format"), `include/screensaver_base.h` (`TWO_LEVEL_COUNT`
  comment: starfield only)
- Modify: `tests/test_helpers/screensaver_test_access.h` (`PipesScreensaverTestAccess`)
- Modify: `tests/unit/test_screensaver_motion.cpp` (pipes cases)
- Rewrite: `tests/unit/test_screensaver_pipes_canvas.cpp`
- Modify: `tests/unit/test_screensaver_fingerprint.cpp`, `tests/fixtures/screensaver/fingerprints.txt`
- Modify: `tests/unit/test_screensaver_canvas_stride.cpp` (the one comment line about ARGB8888)

**Interfaces**
```cpp
class PipesScreensaver : public helix::ui::SaverBase {
  protected:
    std::optional<lv_color_format_t> canvas_format() const override { return helix::ui::SAVER_BUILD_CANVAS_FORMAT; }
    bool on_start() override;   // sim_.init(canvas().frame(), rng(), level()); return true
    void on_frame(uint32_t dt_ms, std::vector<helix::ui::DirtyRect>& dirty) override; // sim_.step
    void on_stop() override;    // sim_.clear()
    size_t ladder_size() const override { return helix::ui::PIPES_LEVEL_COUNT; }
    uint32_t ladder_period_ms(size_t level) const override { return helix::ui::PIPES_LEVELS[level].period_ms; }
    void on_level_request(size_t level) override; // apply_level(level); sim_.set_level(this->level())
  private:
    friend class PipesScreensaverTestAccess;
    helix::ui::PipesSim sim_;
};
// test access
class PipesScreensaverTestAccess {
  public:
    static const helix::ui::PipesSim& sim(const PipesScreensaver& ss);
    static std::array<helix::ui::PipeState, helix::ui::PIPES_MAX_ACTIVE> pipes(const PipesScreensaver& ss);
    /// Same grid occupancy (every node), pipes, yrot and phase.
    static bool same_scene(const PipesScreensaver& a, const PipesScreensaver& b);
};
```
`max_segments`, `total_segments` and `set_total_segments` go: the grid-full shortcut no longer
exists.

- [ ] **Change the tests first:**
  - `test_screensaver_motion.cpp` pipes cases: keep "level 0: configured period, else 16 ms";
    keep "grows one step per 100 ms" and "caps the steps one callback draws" against
    `PipesAccess::pipes()` (`[0]` and `[1]` are alive after start for every seed: 2 is the
    minimum); keep "restarts its seed and step clock on start after stop" and "with the same
    seed replays the same scene" against the new `same_scene`. Delete "drops the steps still due
    when the grid resets" (now Task 7's sim test). In the start-after-stop case, the reset check
    reads `pipes()[0].segment_count == 0` instead of `total_segments == 0`.
  - Add `"the pipes saver declares the pipes ladder"` (periods 16, 33, 50) and
    `"a level request changes the period at once and the scale at the next screen"`:
    `request_level(2)` → timer period 50, `sim(ss).renderer().scale() == 1`.
  - `test_screensaver_pipes_canvas.cpp`: replace both cases with `"the pipes canvas is the
    build's own depth"` (`cf == SAVER_BUILD_CANVAS_FORMAT`) and `"every pipes canvas X byte
    stays 0xFF as pipes grow"` (32 bpp builds; 30 x 100 ms timer runs, as today, but read
    `px[3]` of an XRGB8888 canvas). Rewrite the file's header comment to describe the current
    format, with no account of the old one.
  - `test_screensaver_fingerprint.cpp`: drop the forced reset at frame 61 (the three
    `set_total_segments` lines); the run is otherwise unchanged. Re-record with
    `HELIX_SCREENSAVER_FINGERPRINT_WRITE=1 ./build/bin/helix-tests '[screensaver_fingerprint]'`
    (it records and fails by design), then `make t F='[screensaver_fingerprint]'` without the
    variable: PASS. It skips off the recording platform (`skip_off_the_recording_platform`), so
    record on the same kind of host CI compares on.
- [ ] Run `make t F='[screensaver]'`: expected FAIL (compile: old access members gone).
- [ ] Rewrite `src/ui/screensaver_pipes.cpp` to the wrapper above (about 40 lines, like
  `screensaver_maze.cpp`); delete every `lv_draw_*`, the colour palette, the camera code.
  Rename the constant and update its two users.
- [ ] Run `make t F='[screensaver]'`: expected PASS. Then `make t F='[1591]'` (the stride case
  still covers pipes) and `make t F='[screen_hide]'`.
- [ ] Commit: `git commit -m "feat(screensaver): 3D Pipes draws through PipesSim at the display's depth" -- include/screensaver_pipes.h src/ui/screensaver_pipes.cpp include/screensaver.h include/screensaver_flip_clock.h include/screensaver_base.h tests/test_helpers/screensaver_test_access.h tests/unit/test_screensaver_motion.cpp tests/unit/test_screensaver_pipes_canvas.cpp tests/unit/test_screensaver_fingerprint.cpp tests/fixtures/screensaver/fingerprints.txt tests/unit/test_screensaver_canvas_stride.cpp`

## Task 9: Docs, pixels, gates

**Files:** `docs/devel/SCREENSAVERS.md`.

- [ ] Docs: in "Key files" add `include/screensaver_pipes_sim.h` (grid, growth, renderer,
  dissolve; no LVGL). In "The foundation", pipes runs per `PIPES_LEVELS` (16 ms, 33 ms, then
  33 -> 50 ms at half resolution; scale changes at the next screen). In the flip clock section,
  "its canvas uses the pipes format" becomes `LV_DRAW_CANVAS_FORMAT`. Add a short "3D Pipes"
  section: the original's grid and numbers, 7 pipes a screen, the z-buffer, the fixed light and
  the 16 materials, the dissolve. Present tense, no history.
- [ ] `size build/obj/ui/screensaver_pipes*.o`: `.bss` and `.data` small (no static buffers);
  the teapot control points and materials land in `.rodata`.
- [ ] Headless screenshots, pinned socket (CLAUDE.md recipe), `HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy`:
  ```bash
  TREE=$(basename "$(git rev-parse --show-toplevel)")
  export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
  mkdir -p "$HELIX_CONFIG_DIR"
  HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv \
      --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
  ./build/bin/helix-screen ctl -s "$HELIX_SOCK" click btn_done   # plugin modal, if shown
  ./build/bin/helix-screen ctl -s "$HELIX_SOCK" screensaver pipes
  # screenshots at ~10 s, ~40 s, and mid-dissolve (watch the log for the screen count, or take
  # one every 5 s for 2 minutes), via `ctl screenshot`
  ```
  The before image is `/tmp/claude-1000/-home-pbrown-Code-Printing-helixscreen/41e9fa45-f64c-4947-864e-448345f897fc/scratchpad/before_pipes.png`
  (and `before_pipes_40s.png`). Also one run with `HELIX_SCREENSAVER_LEVEL=2` to show the half
  resolution rung. Open every image before handing it on. Kill only your own PID (resolve it
  from the socket, never `pkill`). Preston judges the look.
- [ ] Gate numbers from that run: the `[ScreensaverManager] pipes used X% of a core against
  Y% budget` lines in the log, reported as-is.
- [ ] `make mutate-diff` once for the branch; give every surviving hunk a test or a stated reason.
- [ ] No translation work: the registry row, name and label are unchanged.
- [ ] Commit: `git commit -m "docs(screensavers): 3D Pipes sim, ladder and canvas format" -- docs/devel/SCREENSAVERS.md`
- [ ] Pi 3B (`pi3b_gate.sh`, `SAVERS="off pipes"`) under a `device:pi3b` claim when it is free;
  otherwise say it was not run.
