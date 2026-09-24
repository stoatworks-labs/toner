# toner

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The copier is not
> asserted but measured: an offline harness drives the real plugin class in a
> headless GL context and reads each claim back out of the picture it made —
> a grey ramp copied n times is the stated tone curve composed n times, column
> for column, with two plateaus and a transition that narrows every copy; a
> solid square's density profile is the strip-field closed form, and its
> centre-to-edge ratio falls with its width while a one-pixel line prints
> full; the drum's defects repeat exactly one circumference apart, at a
> whole-pixel and at a fractional circumference; density down a page of
> constant coverage follows the depletion–recovery closed form; after n
> generations a line has turned by the sum of the per-generation skews; grey
> paper comes out paper white and a grey patch lands where the tone curve
> says — with a negative control per check that proves each can fail. It has
> **never been loaded into Resolume**. It is loaded by
> [oxbow](https://github.com/stoatworks-labs/oxbow), which is a real FFGL host
> and is not Resolume. See [Status](#status).

A photocopier, and a copy of a copy of a copy, as an FFGL effect for
[Resolume](https://resolume.com) Arena and Avenue.

![The test card through four generations with the developer electrode turned down: every solid has gone hollow, a rim of toner round a paper centre, the thick rules have split into double lines, a small square has become a ring, and toner specks repeat down the page](docs/hero.png)

<sub>One frame, rendered by `totest`, the offline harness — not captured from
Resolume. Four generations, Solid Fill 0.3, Toner Supply 0.55, Drum Marks 0.6.
Nothing here is drawn as an outline: every rim is where the fringe field of a
charged area is strong enough to develop, and every hollow is where it is not.</sub>

## The one idea

A photocopier is a xerographic engine, and each stage leaves its mark: optics
that blur, an auto-exposure that throws the background away, a photoconductor
whose tone curve is steep, **development that follows the electric field
above the latent image rather than the image itself**, and a toner supply
that runs short. Copy the copy and the same machine runs again on its own
output. The look of zine and punk-flyer art is the fixed point of that loop.

Toner runs N generations per frame. Each generation is the whole machine
applied to the previous generation's page, with its own small seeded skew and
offset, so the picture is stable from frame to frame. What falls out, none of
it drawn:

- **Tones collapse to black and white.** One generation is an S-curve with
  stable fixed points at paper and at toner and an unstable one at mid-grey;
  composed, it drives every grey to one or the other, faster each copy.
- **Solids go hollow.** The field above a wide charged area is zero at its
  centre (all its flux goes down to the grounded photoconductor) and peaks
  near its edges, so big solids develop as rims round pale centres while a
  thin line's field is its whole width and prints full. `Solid Fill` blends
  in a developer electrode, which flattens the field toward the
  parallel-plate value — exactly what better machines added.
- **Toner starvation.** The developer's concentration falls with the coverage
  it has laid down along the process direction and recovers at a stated rate,
  so density fades down a heavy page and comes back on a light one. Through
  the copy loop a gradient becomes a cut: the page runs out part-way down.
- **Drum marks repeat** every circumference down the page: specks and
  dropouts at seeded positions.
- **Drift and moiré.** Skew and zoom accumulate per generation, and in Photo
  Mode a halftone screen re-screened each copy beats against the last one.

## Controls

| Group | | |
| --- | --- | --- |
| **Machine** | Generations | copies of copies per frame, 1 to 8 |
| | Contrast | the tone curve's slope at mid-grey, 1.6 to 16 |
| | Bg Suppression | how far the AE sensor's reading of the background is taken as paper white |
| | Optics | the lens's Gaussian, none to four pixels |
| | Zoom | 96% to 104% per generation; exactly 100% at the middle |
| | Skew | the largest rotation per generation, up to 2°, with an offset to match |
| **Development** | Solid Fill | the developer electrode: below the window's top every solid hollows over the generations, above it pure black holds |
| | Toner Supply | how fast heavy coverage depletes the developer; 1 is an unlimited supply |
| | Recovery | how fast the developer recovers along the page |
| **Photo** | Photo Mode | a diamond-dot halftone before the drum, re-screened every copy |
| | Screen | 4 to 16 pixels per cell |
| | Screen Angle | 0 to 180° |
| **Page** | Paper | White, Cream, Goldenrod, Pink, Blue, Green, Newsprint |
| | Drum Marks | the defects' strength; 0 is a clean drum |
| | Circumference | the drum's circumference, a tenth of the page to the whole page |
| | Direction | the process direction: Down, Up, Right, Left |
| | Mix | |

## Status

**v0.1.0, local and unreleased, 2026-09-24.** Built from the fleet's templates
in one session. What `tools/verify.sh` establishes on this Mac (Apple M4 Max,
macOS 26.4), on a fresh universal build, at **320×180 and 1280×720**:

| check | what it establishes |
| --- | --- |
| `--fixedpoint` | a ramp copied 1, 2 and 4 times at the default Contrast, 3 at the lowest and 4 at a quarter is the stated tone curve composed that many times, column for column, to 3×10⁻⁶ at 320 wide and 3×10⁻⁵ at 1280 against tolerances of 3×10⁻⁶ to 4×10⁻⁴ derived from the GPU's `exp` through the curve's slope; two plateaus every time; the transition narrows 66 → 15 → 2 columns at 320 wide, 256 → 52 → 4 at 1280 |
| `--fringe` | the density profile through a 3, 5, 9, 13, 17, 25, 33 and 49 px square matches the strip-field closed form to 5×10⁻⁶ (tolerance 1.7×10⁻⁴); the centre/edge ratio goes 1, 1, 1, 1, 0.41, 0, 0, 0; nothing develops two pixels outside an edge; a one-pixel line develops solid; Solid Fill 1 leaves the 33 px square's centre solid |
| `--drum` | repeats of every clear speck are one circumference apart to 8×10⁻⁶ px at a whole-pixel circumference (99 and 396 px) and 5×10⁻⁵ px at a fractional one (66.6 and 266.4 px), tolerance 10⁻³ px; the whole-pixel windows agree pixel for pixel to 2×10⁻⁶ |
| `--starvation` | density down a page of half coverage follows c∞ + (1 − c∞)(1 − λ)ʲ at 0.04 of a per-row float tolerance, in four settings and three process directions, including no recovery at all |
| `--skew` | after 2, 5 and 8 generations a line has turned by the sum of the seeded skews: 0.880°, 1.540° and 3.161° measured at 1280 wide against 0.888°, 1.514° and 3.132°, within a tolerance of atan(1.5/span) — 0.13° there, 0.54° at 320 wide |
| `--ae` | grey paper at 0.55 comes out exactly white at full suppression; a 0.30 patch lands on T(0.30/0.55) = 0.71984 to 4×10⁻⁵, and at half suppression a 0.42 patch on T(0.42/0.775) = 0.70353 to 1.3×10⁻⁵ |
| `--negative` | six perturbed copiers — a plain threshold for the field, one skew for every generation, an AE sensor reading the page mean, a developer depleting by demand, drum repeats 1% too far apart, a linear photoconductor — each **fails** its check at both rasters |
| mutation | one character of the shipped GLSL (the discharge curve's residual, `- PidcFloor` → `+ PidcFloor`) was caught by all six checks at both rasters, then reverted |
| `tools/sweep.py` | all **17** controls measurably change the picture |
| shaders | all 13, as the plugin compiles them, through `glslc` |
| `--pipe` | 2.5 frames in, exactly 2 out; an unknown cue refused (2); a failed render and a closed stdout (`\| head -c 1`) each exit 1 |
| the bundle | universal (`x86_64 arm64`), exports `plugMain`, ad-hoc signs; `oxbow` reports `SW Toner` / `TO01` / `effect` and renders 120 frames through `plugMain` |

Render cost, best of three runs of 60 frames after a warm-up, `glFinish` both
sides, on a GPU shared with other builds, at Generations 1 / 5 / 8:
**0.8 / 4.0 / 6.3 ms** at 720p, **1.4 / 6.5 / 10.2 ms** at 1080p,
**4.6 / 21 / 34 ms** at 4K. So the cap of 8 generations keeps 1080p inside a
60 fps frame (62% of it); at 4K five do not, and three should by interpolation
(about 13 ms) but were not timed.
Each generation is eleven passes, and the two 33-tap field passes are most of
the cost. macOS figures only.

Seen on footage: eight of Resolume's bundled demo clips through `--pipe` at
the defaults, judged by eye — dark clips come out as black pages with the
bright shapes cut out in paper, lit scenes as line-and-solid drawings; none
floods and none blanks. Not measured.

### Not established

It has **never been loaded into Resolume**, on either platform. Everything
above was compiled, rendered and measured offline against the real plugin
class in a headless CGL context, plus an `oxbow` load. How seventeen controls
read in Arena's inspector is untested. Windows compiles in CI's design and
has not been built here. No OpenFX port and no browser demo, neither in scope
for 0.1.0. No user guide yet.

## Build

Needs CMake 3.15+, a C++17 compiler, and the FFGL SDK submodule.

```bash
git clone --recursive https://github.com/stoatworks-labs/toner
cd toner
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build     # into ~/Documents/Resolume Arena/Extra Effects
```

macOS builds are universal (Apple Silicon + Intel) by default; add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster dev build. Windows needs GLEW via vcpkg.

## Building and testing

The offline harness renders the real plugin class headlessly:

```bash
./build/totest --out /tmp/frame.png --size 1920x1080   # the moving card
./build/totest --list                                  # every control, kind and default
./build/totest --fixedpoint --fringe --drum --starvation --skew --ae   # each claim, measured
./build/totest --negative                              # and the checks can fail
./build/totest --offline                               # what needs no GL (CI)
./build/totest --bench                                 # 720p, 1080p and 4K at 1, 5 and 8 generations
python3 tools/sweep.py                                 # no control is silently dead
tools/verify.sh                                        # all of it, on a fresh universal build
```

Every check takes `--size`; run it at 320×180 as well as the raster you care about.
Footage goes through the real shaders with `--pipe`, in the fleet's frame format:

```bash
ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
  | ./build/totest --pipe --size 1920x1080 --script cues.txt \
  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 50 -i - out.mov
```

See [`CLAUDE.md`](CLAUDE.md) for the full command reference and
[`AGENTS.md`](AGENTS.md) for the model and the traps.

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT — see [LICENSE](LICENSE).
