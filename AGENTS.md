# AGENTS.md — Toner

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

A photocopier — and a copy of a copy of a copy — as an FFGL 2.1 effect (`TO01`,
shown as `SW Toner`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake,
universal macOS `.bundle` and a Windows `.dll`. MIT; home
`github.com/stoatworks-labs/toner`, released at v0.1.0 on 2026-09-24 with a user
guide, a browser demo and a project video.

Built 2026-09-24 in one session from the fleet's templates and `specs/SPEC-toner.md`
(with `BRIEF.md` and `BRIEF-ADDENDUM.md`): slope (by way of clamp) for the harness,
the verify script, the `--pipe` contract, the negative-control pattern, `--offline`
and the GL-less CI; rosette for the halftone's spot-function thinking; tinsel for
`PassBuffer` and the trap list; graticule for the notes and the provisional About.

---

## The one idea

**Each copy is the whole xerographic engine run once on the last copy's page, and
toner follows the FIELD above the latent image, not the image.**

N generations a frame. Generation g takes generation g − 1's page (the source's luma
for g = 1) through, in order:

| stage | what it does | where |
| --- | --- | --- |
| place | zoom Z, this generation's seeded skew θ_g and offset d_g about the centre; bilinear by hand; off the page is paper | `kPlace` |
| optics | a separable Gaussian of `Optics` pixels | `kBlur` ×2 |
| auto exposure | B = the mean of the brightest sixteenth of a 64×36 grid of point samples; the page is scaled so mix(1, B, `Bg Suppression`) is paper | `kAE`, `kLatent` |
| screen | Photo Mode only: diamond dots whose area is exactly the tone, closed form | `kLatent` |
| photoconductor | q(R) = (e^(−kR) − e^(−k)) / (1 − e^(−k)), k = 2: the discharge curve, 1 at black, 0 at paper | `kLatent` |
| develop | E = q ⊛ (C_h ⊗ C_h) − q ⊛ (C_H ⊗ C_H), Cauchy kernels at the gap h and at h + 2t (the image charge under the grounded photoconductor); gain G so a 1-px line's field is 1; the electrode: E′ = (1 − s) G E + s q; toner D = clamp((E′ − lo)/(hi − lo)) with the window centred on q(½), width dq/dR ÷ Contrast | `kFieldX`, `kDevelop` |
| starvation | c₀ = 1, c_{j+1} = c_j + r(1 − c_j) − κ a_j c_j down the process direction, a_j the row's mean density; the row is laid at D c_j | `kCoverage`, `kSupply`, `kMarks` |
| drum | eight seeded defects per circumference, Gaussian specks (added) and dropouts (multiplied), at u₀ + mC | `kMarks` |
| fuse | a separable 3-tap [s, 1 − 2s, s] spread; page out = 1 − density | `kFuse` |

With the electrode fully in, one generation is the S-curve T(R) = 1 − D(q(R)) with
stable fixed points at 0 and 1 and an unstable one at mid-grey, and:

| what the machine does | what comes out |
| --- | --- |
| the S-curve composed n times | **greys collapse** to paper or toner, the transition shrinking by the slope each copy |
| a wide charged area's field is zero at its centre, strongest near its edges | **solids go hollow**; a thin line's field is its whole width and prints full |
| the developer depletes with coverage laid and recovers along the page | **the page runs out** part-way down; through the loop the gradient becomes a cut |
| defects repeat every circumference | **drum marks** in a column down the page |
| θ_g and Z compose | **drift**; in Photo Mode the screen re-screened beats against itself |

### What does not fall out, and is the honest limit

- **The field is a separable product of 1-D Cauchy kernels**, not the radial Poisson
  kernel of a point charge over a plane. It is exact for infinite strips and for the
  centre of any rectangle (a product of two strip closed forms) and is what the harness
  can state; off-axis near a small mark its shape differs from the radial one, and
  at a gap of 1.5 px it even got the sign wrong two pixels outside a 3-px square (a
  ring of toner where the radial field would have cleaned). At half a pixel the near
  kernel is close to a delta and that artefact is gone; it is not proven absent
  everywhere.
- **The gap, the thickness and the kernel radius are in pixels**, not page fractions,
  so a 4K page hollows features half the size, relative to the page, that a 1080p
  page does. Optics is in pixels by the spec, and the field followed it.
- **The kernels are truncated at 16 texels** and renormalised, so a solid wider than
  33 px has exactly no field more than 16 px from its edge. The Cauchy tail beyond
  (6% of the wide kernel) is gone; the hollowing runs from about 13 px to 25 px and is
  complete past that, rather than fading as 1/width for ever.
- **The developer starts every generation fresh** (c₀ = 1). A real machine's developer
  carries the last page's state into this one.
- **The AE sensor is a statistic, not a mode.** "The histogram's upper mode" in the spec
  became the mean of the brightest sixteenth of the page, because a mode needs a rule
  for empty bins and this needs none; on paper with text the two agree.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Model.h` | The machine, described; the constants; the `Perturb` bits; the option tables; the hash and the seeded tables (skew per generation, drum defects); the closed forms the plugin computes on the CPU (charge, window, kernels, gain). |
| `source/Controls.{h,cpp}` | Every slider to its physical unit. |
| `source/Shaders.{h,cpp}` | Twelve fragment shaders and the vertex shader. **The shaders are the copier.** |
| `source/Toner.{h,cpp}` | The plugin: parameters, buffers, the coefficients in double, eleven passes a generation, the test hook. |
| `source/PassBuffer.*` | tinsel's FFGLFBO with the leak fixed. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/totest/` | The offline harness: renders, measures, benchmarks, pipes, dumps shaders. |
| `tools/check-shaders.sh` | glslc on the dumped shaders; verify.sh and CI both call it. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/verify.sh` | All of it, at two rasters, plus the release-time checks done locally. |
| `demo/` | The browser demo: `plugin.js` holds the plugin's fourteen shader bodies verbatim (assembled as `Shaders.cpp` assembles them) and a hand PORT of the CPU half (`Controls.cpp`, the closed forms and seeded tables of `Model.h`, the per-frame coefficients and pass order of `Toner::ProcessOpenGL`); `tools/check_shaders.py` keeps the shaders identical (verify.sh runs it); `vendor/` is the shared kit from `stoatworks-backend/resolume-demo` (never edit it, re-run `sync.sh`). Served by this repo's own Worker at `toner-demo.stoatworks-labs.com` through a DNS record + route (the zone is out of custom domains); `deploy.yml` redeploys it on a push to main. Generations is a dropdown there (the kit has no integer type). Measured once against `totest --pipe` on the same 960×540 colour-bars frame: every pixel within 1/255. |

Buffers: two page buffers (R32F, ping-ponged per generation), two work buffers
(R32F), the field's x pass (RG32F, both kernels), a 1×1 AE reading, and two L×1
buffers along the process direction (coverage and supply). All Nearest; every read is
`texelFetch`. Every coefficient — blur weights, the two kernels, the window, the
placement matrix, the defect table — is computed on the CPU in double once a frame.

---

## Traps

Roughly in the order they will bite.

### ☠️ The gain a 1-px line needs saturates everything until the field's scale is sub-pixel

The first field had the gap and the thickness at 1.5 px each and 49 taps. The profile
matched the closed form to 10⁻⁷ — and the centre/edge ratio was 1.000 for every
square up to 33 px and 0 at 65, because a wide area's field under those kernels
decays as ~1/width from a peak that, after the gain that makes a 1-px line print,
sits at 2.4× solid: nothing fell below the window's top until the truncation radius,
where the field drops to exactly zero. The picture showed it as a hard 24-px frame
round a white interior, which is a border, not a fringe. At half a pixel each the
line-to-area ratio is right: 13 px solid, 17 px at 0.41, 25 px hollow. Physically a
20 µm photoconductor under a 300 mm page at 1080 rows is a fifteenth of a pixel, so
half is already generous.

### ☠️ Solid Fill below the window's top makes every solid vanish over the generations

The interior of a wide solid develops at `Solid Fill × charge` (the fringe term is
zero there). With Solid Fill 0.35 and the window's top at 0.375, a full-charge
interior develops to 0.88, the next copy sees 0.12 of reflectance… and by the third
it is paper. On the eight demo clips that read as **near-blank white pages**: the
opposite of tranche three's flood and just as unusable. Above the top (the default is
now 0.5) pure black holds and the clips came out as black pages with the bright
shapes cut out in paper, which is the look. So Solid Fill is not a subtle control:
across the window's top it decides whether the page keeps its solids at all.

### ☠️ Starvation becomes a cut, not a gradient, through the copy loop

One generation lays c(y), a gradient. The next generation's S-curve maps the depleted
grey to paper or toner, so by the third copy the page is black to the row where c
crossed the interior's survival level and white below it. With Toner Supply 0.7 a
black frame came out as a black bar across the top fifth and nothing else. The
default is 0.85, at which a full page's c∞ (0.92) stays above that level and the page
holds; pull it down and the page runs out part-way, abruptly. Physical, but not a
default.

### The rotation's handedness

The first place pass rotated the source lookup by +θ, reasoning that GL's y-up frame
flips the sense. It does not: the picture is upright in GL's frame, a standard
rotation matrix turns it anticlockwise as seen, and `--skew` measured exactly the
negative of the prediction. The inverse map is R(−θ).

### Generation 1's seeded skew is a thousandth of the maximum

`SkewFraction(1)` happens to be −0.001. A one-generation skew check therefore
measures nothing, and reusing skew 1 for every generation (the spec's negative
control) gives ~0° — distinguishable from the true sum only where the sum is large.
The check runs 2, 5 and 8 generations and says, per case, whether the negative
control could be told apart there; at 320×180 the 2-generation case cannot.

### A 1-px line's neighbours develop too

The first line check expected the line's own column at solid × (1 − 2s) after the
fuse spread and its neighbours at s each. But the field one pixel outside a
full-charge line is above the window's top, so the line develops three pixels wide
and the spread reads differently. The check now predicts the line with the same
closed form as the squares (a box of half-width 0 across, the whole kernel along).

### The separable kernel's off-axis sign

With the 1.5-px kernels, two pixels outside a 3-px square the product form gave a
positive field (the near kernel's along-edge factor outweighed the far one's) and a
ring of toner developed where the radial field would have cleaned. At half a pixel
the near kernel is nearly a delta and the sign is right; `--fringe` asserts nothing
develops two pixels outside every square's edge.

### The page is opaque, and the demo clips said so

The composite kept the host's alpha, and every one of Resolume's bundled demo clips
has an alpha channel, so through `--pipe` at 1080p a page was paper and toner only
where the clip was opaque. A copier outputs a sheet: the composite writes alpha 1,
and Mix fades back to the source alpha included. Found by the video survey, fixed
before the tag; no harness check reads alpha, so none noticed.

### Screen Angle's ends are the same picture

A square dot lattice is the same lattice a quarter turn on, so 0° and 180° render
identically and the sweep reported the control dead. It compares 0° with 45°.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and
restored before the composite, with the host's FBO bound explicitly); every
`ffglex::Scoped*` clears to 0 on exit, so every `Ensure()` happens before anything
binds a texture, and the pass loop uses raw binds it clears itself;
`FFGLFBO::Release()` leaks the colour texture (`PassBuffer::Destroy()` deletes it
first); `SetParamInfo` clamps a STANDARD default into 0..1 and `SetParamInfof` reads
its default out of `params[]`; an option's range reads back 0..1 whatever its element
count; the core is an **OBJECT** library; `SetTextParameter` must return `FF_SUCCESS`
for the About block; `FFGLShader::Set` has no array overload (`glUniform1fv` for the
kernels, the weights and the defects); `nm | grep -q` fails under pipefail when grep
succeeds; a closed stdout must be a failed write, so `--pipe` ignores SIGPIPE; zsh
has no `PIPESTATUS`, so `verify.sh` is bash. Resolume's clock overflowing a float
does not arise: the plugin has no clock.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every check ran at
320×180 and 1280×720 in `verify.sh`.

What makes them rasteriser-proof by construction: **every coordinate is an integer
computed in integers** (`gl_FragCoord`, never an interpolated uv, except the
composite's read of the host's picture); **every read is `texelFetch`**, the
placement's bilinear included, so an identity placement is exactly the identity;
**every coefficient is computed on the CPU in double** and handed over as a float
uniform, and the harness's statement of each law is rounded through the same float;
the only GPU transcendentals in a checked path are one `exp` per pixel (the discharge
curve; GLSL 4.10 §8.2 allows 3 ulp) and one per defect (the drum's Gaussian).

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--fixedpoint` values | every column of a ramp after n copies against a 1-D run of the stated curve and spread | ε Σ_{i<n} Sⁱ with ε = 3 ulp of `exp` over (1 − e⁻²) over the window's width + 4 ulp: the exp error once, amplified by the slope S per further generation | none in the tolerance; the measured worst is 3×10⁻⁶ at 320 and 3×10⁻⁵ at 1280, both far inside |
| `--fixedpoint` plateaus | maximal runs within 10⁻³ of 0 or 1 | exactly 2, both sides; transition width ±2 columns (one column at each end can sit within 10⁻³ of the threshold) | the transition is W/5 columns at n = 1, so wider pages see more of it |
| `--fringe` profile | the centre row through a square against the product of two strip closed forms through the window and a 3×3 spread | 2 × 33 × 2 × 2 ulp × G + 3 ulp × G, over the window's width, + 4 ulp: two float sums of 33 terms below 1, twice, through the gain | none; squares need 2a + 1 + 2R + 4 ≤ min(W, H) and the 49-px case runs at both |
| `--fringe` ratio | centre/edge over eight widths | falls wherever the closed form falls by more than 4 tolerances; measured monotone | none |
| `--drum` spacing | consecutive centroids of a speck's repeats | 10⁻³ px: a Gaussian of σ ≥ 1 sampled at pixel centres has its centroid within e^(−2π²σ²) of the truth, the spread is symmetric, and the float error over an 81-px window is < 10⁻⁵ px | more repeats on a longer page (3 and 6 at 180 rows, more at 720) |
| `--drum` windows | two windows a whole circumference apart, pixel for pixel | 4 ulp(L) × e^(−½)/σ_min + 8 ulp: the rounding of (u − u₀ − mC) at the page's scale through the Gaussian's largest slope | the ulp of L grows with the page: 3.75×10⁻⁵ at 180 rows |
| `--starvation` | density at every 7th column of every row against c∞ + (1 − c∞)(1 − λ)ʲ through the spread | 2 ulp × (3(j + 1) + 4) + 4 ulp: three roundings a row of a value ≤ 1, plus the spread's | grows linearly with the row count, as the recurrence does |
| `--skew` | a least-squares angle from per-column density centroids over the middle half | atan(1.5/span): a deviation bounded by ±½ px moves an LSQ slope by at most 1.5/span (0.5 · (n²/4)/(n³/12)) | 0.54° at 320 wide, 0.13° at 1280; the measured error was 0.2° and 0.03° |
| `--ae` | the paper region and two patches against T(level / mix(1, B, s)) | (150 × 2 ulp × S + 3 × 2 ulp) / width + 4 ulp: B is a float mean of 144 samples, the division and the mix two more roundings, the exp three ulp, all through the curve's slope | none |
| `--laws` | every control law at 21 points; the model's promises | 10⁻¹² relative; exact | none (no GL) |

Deliberately NOT relied on: `mix(a, b, 1) == b` (the checks that need the electrode
fully in tolerate a rounding); exact cancellation in the charge at paper (the window's
lower edge is 0.16 above zero); round-to-nearest anywhere; interpolated varyings; a
texture unit's filtering; GLSL integer division of a negative operand (none occurs);
the 8-bit readback (the harness reads floats).

What might still differ on another rasteriser: a driver whose `exp` is worse than 3 ulp
would eat into `--fixedpoint`'s margin at four generations (the measured worst is a
sixteenth of the tolerance); `check-shaders.sh` covers the syntax on a second compiler
and the rendered checks run with `--allow-no-gl` so a runner without a context skips
loudly.

### The negative controls

`totest --negative` runs six against the rendered checks; `--perturb BITS` runs any
check verbosely against one. Each perturbs the *plugin's* shaders or uniforms — a
`Perturb` bitmask the shipped plugin carries at zero — never the harness's expectation.

| perturbation | what fails, measured at 320×180 |
| --- | --- |
| the field kernel replaced by a plain threshold on the charge | `--fringe`: 5 assertions — every square's centre solid, the ratio flat at 1 |
| one skew reused for every generation | `--skew`: all three cases, ~0° against 0.89°, 1.51°, 3.13° |
| the AE sensor reading the page mean | `--ae`: 3 assertions — the 0.30 patch at 0 against 0.72, the paper at half suppression wrong |
| the developer depleting by the demand, not what it laid | `--starvation`: all four settings, c going negative |
| the drum's repeats 1.01 circumferences apart | `--drum`: both cases, spacing off by a hundredth of C |
| a linear photoconductor | `--fixedpoint`: 15 assertions — one plateau, no transition |

### The mutation

One character of the shipped GLSL, on a clean committed tree: in the latent pass,
`( exp( -Pidc * Rn ) - PidcFloor )` → `( exp( -Pidc * Rn ) + PidcFloor )`, leaving
paper with a charge of 0.31 instead of 0. Caught at 320×180 and 1280×720 by **all six
checks**: `--fixedpoint` (worst column error 1.0, one plateau), `--fringe` (profiles
off by 0.5, the ratio falling at the wrong width), `--starvation` (5×10⁵
tolerances), `--skew` (~0.1° measured against 0.9°–3.1°: the line drowned in a
toner haze), `--ae` (the paper at 0.24 and 0 against 1) and `--drum` — the fractional
case only, by 0.57 px: a uniform haze biases each window's centroid toward the
window's centre, which cancels between two whole-pixel repeats (same sub-pixel phase)
and not between two fractional ones. Reverted with `git checkout source/Shaders.cpp`;
the tree was clean before and after; the checks passed again on the rebuilt binary.

---

## Decisions taken without asking

- **The AE statistic** is the mean of the brightest sixteenth of a 64×36 grid of point
  samples, floored at 1/32 — not the upper mode — because a mode needs a rule for
  sparse bins and this does not; on paper with text they agree.
- **The field's gain** makes a 1-px line's field equal to the parallel-plate field of a
  full-charge solid (1.0); the spec's "a 1-pixel line develops to full density" then
  holds by a factor of 2.7 over the window's top rather than at its edge.
- **Gap and thickness are 0.5 px, the kernel radius 16** (above).
- **The electrode blends fields**, E′ = (1 − s) G E + s q, and the development window
  applies to the blend. Solid Fill default 0.5, above the default window's top.
- **The tone curve's unstable fixed point is mid-grey**, and Contrast is its slope
  there, 1.6 × 10ᵛ; 1.6 is just above the slope at which the window's lower edge would
  reach zero charge (1.58).
- **Starvation's rates are per page**, divided by the row count, so the profile is the
  same in page fractions at every raster; the developer starts every generation at
  c = 1; a row is laid at the concentration before its own depletion. Toner Supply
  default 0.85 (above).
- **Positive Skew is anticlockwise as seen.** The offset amplitude rides on the Skew
  slider (half a percent of the shorter side at 1): one control for the drift.
- **Drum marks are Gaussian specks and dropouts**, σ 1–3 px, five specks and three
  dropouts per circumference, applied to the density before the fuse; a nearest-repeat
  evaluation, valid because a circumference (≥ 18 px) is far larger than a mark.
- **Circumference is a fraction of the page** (0.1 + 0.9v), not pixels, so the whole-
  pixel drum check picks a slider whose float circumference is whole and says so.
- **The halftone is diamond dots** with the exact closed-form area law (2T² below a half,
  1 − 2(1 − T)² above), no rank table; the tone is the pixel's own, not the cell's mean.
- **Screen Angle spans 180°** although 90° would do; the sweep knows.
- **Off the page is paper**, not the black of an open lid.
- **Zoom is 1 + 0.08(v − ½)**, exactly 1 at the middle.
- **Generations 1..8**, a real integer; 8 is where 1080p is still inside a frame.
- **No clock, no state across frames**, so no resize check; documented in CLAUDE.md.
- **`--fps` is accepted and does nothing**, so the fleet's video renderer can pass it.
- **`--fail-render-at N`** is a harness-only hook so `verify.sh` can prove `--pipe`
  exits 1 on a failed render.
- **The hero is rendered off the defaults** (Solid Fill 0.3, Toner Supply 0.55,
  Generations 4, Drum Marks 0.6) because the defaults on the synthetic card show a
  clean threshold; the caption says so.
- **Provisional About and attributions** (`StoatworksAbout.h`, `ATTRIBUTIONS.md`) are
  hand copies adapted from slope's with `guide=""`, so three About buttons; the
  release step registers the project and re-runs the syncs.
- **The FFGL submodule was dissociated from the reference clone** (`repack -a -d`, the
  alternates file removed) so this repo does not depend on a path in `~/Projects`.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4 (2026-09-24)

Every number is `tools/verify.sh` on this machine against a fresh universal Release
build, at 320×180 and 1280×720.

- **Fixed point.** Five cases (Contrast 0.5 at 1, 2 and 4 generations; 0 at 3; 0.25 at
  4): every column within 3×10⁻⁶ (320) and 3×10⁻⁵ (1280) of the composed curve
  against tolerances of 2.7×10⁻⁶ to 4.4×10⁻⁴; two plateaus each; transitions 66 → 15
  → 2 and 256 → 52 → 4 columns, 168/669 at the lowest contrast.
- **Fringe.** Eight squares: profiles within 5×10⁻⁶ of the closed form (tolerance
  1.7×10⁻⁴); ratio 1, 1, 1, 1, 0.411, 0, 0, 0; nothing two pixels outside; the 1-px
  line's profile to 3×10⁻⁸ with the closed form at exactly 1; the electrode leaves
  the 33-px centre at 1.000000.
- **Drum.** Whole-pixel: 3 (99 px) and more (396 px) consecutive repeats within
  8×10⁻⁶ px, windows agreeing to 2×10⁻⁶; fractional: 6 (66.6 px) and 8 (266.4 px)
  within 6×10⁻⁵ px.
- **Starvation.** Four settings, three directions: worst 0.045 of tolerance.
- **Skew.** 0.892°/1.603°/3.335° at 320 and 0.880°/1.540°/3.161° at 1280 against
  0.888°/1.514°/3.132°.
- **AE.** Paper exactly 1.0 at full suppression; patches within 4×10⁻⁵ and 1.3×10⁻⁵.
- **Negative controls.** All six fail their check, at both rasters.
- **Mutation.** Caught by all six (above).
- **No dead controls**, all 17, with the four About buttons skipped.
- **Every shader compiles** through `glslc`, all 13, as the plugin hands them to the
  driver.
- **`--pipe`** returns exactly two frames for two and a half, refuses an unknown cue
  with 2, and exits 1 on a failed render and on a closed stdout (`| head -c 1`).
- **The bundle** is universal, exports `_plugMain`, carries
  `com.stoatworks.ffgl.toner`, ad-hoc signs, and `oxbow` reports `SW Toner` / `TO01`
  / `effect` and renders 120 frames through `plugMain`.
- **Render cost**, best of three runs of 60 frames after a warm-up, `glFinish` both
  sides, on a shared GPU, at Generations 1 / 5 / 8:

  | | ms/frame | % of a 60fps frame |
  | --- | --- | --- |
  | 1280×720 | 0.82 / 3.95 / 6.32 | 5 / 24 / 38 |
  | 1920×1080 | 1.44 / 6.50 / 10.29 | 9 / 39 / 62 |
  | 3840×2160 | 4.58 / 21.2 / 33.6 | 28 / 127 / 201 |

  The cap of 8 keeps 1080p real-time. At 4K, three generations should by
  interpolation (~13 ms) and were not timed. Two 33-tap passes a generation are most of
  it.
- **On footage, by eye only:** eight of Resolume's bundled demo clips (Beat 001, Bass
  003, Synth 004, Trinity_09, IntoTheGlow_02, OrganicMotions_06, BattleWeapon_Tank_09,
  FogAndDust_3) through `--pipe` at the defaults. Dark clips come out as black pages
  with the bright shapes cut out in paper and the fringe edging the greys; the lit
  scene as a line-and-solid drawing. None floods, none blanks. The two earlier
  defaults that did (above) were found this way.

### Assumed, or not done

- ☠️ **Never loaded into Resolume on macOS.** Everything was compiled, rendered and
  measured offline against the real plugin class in a headless CGL context, plus an
  `oxbow` load.
- On Windows, in Resolume Arena 7.27.1 (win-lab, Mesa llvmpipe, no GPU, 2026-09-24): this release's DLL loads from Extra Effects, registers as `SW Toner` / `TO01` / effect, all 23 host controls match the declaration, it renders and Arena's log stays clean: 9 of the fleet gate's 9 checks, with all 18 controls moving the picture (four under a precondition: Screen and Screen Angle in Photo Mode, Circumference with Drum Marks up, Direction with Toner Supply down). Software rendering says nothing about a GPU or about speed.
- **Footage judged by eye**, not measured; eight clips, one frame each.
- **Not verified at 4K**, only benchmarked there.
- **Windows** is built by CI (the first run failed on MSVC: `M_PI` and a variable
  named `far`, both fixed before the tag).
- **The fringe's off-axis shape** is not proven against the radial kernel anywhere;
  only its on-axis profile and the two-pixels-outside cleanliness are checked.
- **No OpenFX port**, not required for 0.1.0. The browser demo's CPU half is a port
  that only a reader checks; its one measurement is the single frame above.
- **`StoatworksAbout.h` and `ATTRIBUTIONS.md` are generated** by the backend's
  `sync-about.py` and `sync-attributions.py`; do not hand-edit them.
- **Nothing has been through a show.**

---

## Open questions

- **Should the field's scale follow the page rather than the pixel?** A page-fraction
  gap would make 4K hollow the same features as 1080p, at four times the taps per
  pixel times four times the pixels. As it is, the operator gets the look at the
  raster they tune on.
- **Should the electrode be a gain on the fringe rather than a blend toward the plate
  field?** The blend is what makes Solid Fill a survival switch across the window's
  top; a different coupling might make it a gentler control.
- **Should the developer carry its state across generations** (c at the end of one page
  starting the next) as a real machine does? One constant's worth of code; a different
  starvation law to state.
- **Should there be a separate Offset control?** It rides on Skew now.
- **Should Circumference be in pixels** so the whole-pixel case is the operator's to
  choose? A fraction is the physical truth and the raster-independent one.
- **A radial field kernel** (the true Poisson pair) is not separable; a 2-D
  convolution at radius 16 is 1089 taps and would need a coarser grid or an FFT.

---

## Siblings

- **slope**, by way of **clamp** — the harness, verify, CI and `--pipe` shapes, the
  negative controls, `--offline`, the AGENTS.md shape.
- **rosette** — the halftone as a spot function with an exact area law.
- **rebate** — a process chain with a characteristic curve, stated in one header.
- **escapement** — repeated passes through one system; the integer hash.
- **tinsel** — `PassBuffer`, `sweep.py`, and the fleet's trap list.
- **graticule** — the notes, and the provisional About.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
