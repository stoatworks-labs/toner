# toner

A photocopier, and a copy of a copy of a copy, as an FFGL **effect** for
Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) +
Windows `.dll`. MIT.

Read `AGENTS.md` before changing the model (`Model.h`, the shaders in
`Shaders.cpp`), the control laws, the defaults or the harness's tolerances.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships, and what `verify.sh` builds): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel 4`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/totest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Generations=5" --set "Solid Fill=0.3" --set "Paper=2"`
  (0..1 for sliders, the element index for options, the integer for Generations)
- List parameters, kinds, defaults and ranges: `./build/totest --list`
- The exact GLSL the plugin compiles: `./build/totest --dump-shaders DIR`
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (accepted for the fleet's contract; this plugin has
  no clock, so it changes nothing) and an optional `--script` of
  `frame Parameter Name value` cues, linearly interpolated between a name's cues and
  held before the first and after the last — an option index interpolated passes
  through the options between, so key a cut two cues a frame apart. A cue naming no
  parameter exits 2 before any frame; a partial frame at the end of stdin ends the
  stream with exit 0; a failed render or a closed stdout exits 1 (SIGPIPE is
  ignored so a closed stdout is a failed write, not a 141):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/totest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + the offline checks +
  every rendered check at 320x180 AND 1280x720 + the --pipe contract + the sweep + the
  bundle, ~5 min)
- A ramp copied n times is the tone curve composed n times: `./build/totest --fixedpoint`
- A solid's field against the strip closed form; a 1-px line prints full: `./build/totest --fringe`
- The drum's repeats are one circumference apart: `./build/totest --drum`
- Density down a page follows the depletion-recovery closed form: `./build/totest --starvation`
- A line turns by the sum of the per-generation skews: `./build/totest --skew`
- Grey paper comes out white; a patch lands at T( patch / paper ): `./build/totest --ae`
- The checks can fail: `./build/totest --negative`; one perturbation verbosely:
  `./build/totest --fringe --perturb 1` (bits in `Model.h`)
- No GL (what CI runs first): `./build/totest --offline` = `--laws --names`
- Every rendered check takes `--size WxH`; CI runs them at 320x180 with `--allow-no-gl`
- Shaders through glslc: `tools/check-shaders.sh build/totest`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/totest --bench` (Generations 1, 5, 8 at 720p, 1080p, 4K;
  best of three; the GPU is shared, so run it twice)
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Toner.bundle`
- The browser demo's shaders are the plugin's, character for character: `python3 demo/tools/check_shaders.py`
  (in verify.sh). The demo's CPU half (`demo/plugin.js`) is a hand port; only a reader checks it.
- Deploy the demo: `cf-run npx wrangler deploy` from the repo root (a push to main also deploys it);
  verify by content: `curl -s 'https://toner-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`

## Notes
- **The shaders ARE the copier.** Each stage lives once, in GLSL (`Shaders.cpp`); the
  C++ converts sliders to uniforms (`Controls.cpp`), computes the coefficients in
  double (`Toner.cpp`) and runs the eleven passes per generation. The harness restates
  the model from `Model.h`'s description and holds the shaders to it.
- **Nothing carries across frames** and there is no clock (`SetTimeSupported(false)`):
  each generation's skew and the drum's defects are seeded by index. A frame is the
  same picture every time, so there is no resize check to run.
- **Every read is `texelFetch` at integer coordinates.** The placement does its own
  bilinear from four fetches, so an identity placement (Zoom 0.5, Skew 0) passes the
  page through exactly — which is what lets the tone-curve checks see the curve alone.
- **The field's scale is half a pixel** (`kGapPx`, `kThicknessPx`) and the kernels are
  33 taps. At a pixel and a half the wide-area field decayed so slowly against the gain
  a 1-px line needs that nothing hollowed until the truncation radius. AGENTS.md.
- **Solid Fill against the window's top decides whether pure black survives** the copy
  loop: the interior of a wide solid develops at `Solid Fill x charge`, so below
  `hi` (0.375 at the default Contrast) every solid hollows into a rim over the
  generations, and above it pure black holds. The default 0.5 holds; 0.3 hollows.
- **Starvation becomes a cut through the loop.** One generation lays a gradient; the
  next generation's S-curve pushes the depleted grey to paper. The default Toner
  Supply (0.85) keeps a black page black; lower it and the page runs out part-way.
- **Positive Skew is anticlockwise as seen**; the inverse map in the place pass is the
  rotation by -theta over the zoom. Generation 1's seeded skew is tiny (-0.001 of the
  maximum), so a one-generation skew check proves nothing; the harness uses 2, 5, 8.
- **Screen Angle's two ends are the same lattice** (a square lattice has a quarter-turn
  symmetry), so the sweep compares 0 with 45 degrees rather than 0 with 180.
- **Parameter names must be unique and 16 characters or under** — hence `Bg
  Suppression` and `Circumference`.
- `SetParamInfo` clamps a STANDARD default into 0..1; `SetParamInfof` reads its default
  out of `params[]`, so fill `params[]` first. Options are mapped by index in
  `Controls.cpp` (an option's range reads back 0..1); Generations is a real
  `FF_TYPE_INTEGER` with `SetParamRange(1, 8)`.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `toner_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include `<ffglex/FFGLScopedFBOBinding.h>`.
- `FFGLShader::Set` has no array overload: the blur weights, the two kernels and the
  defect table go in with `glUniform1fv`/`glUniform1iv` under the pass's shader binding.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `TO01`, display name `SW Toner`.

## Not done yet
- **Never loaded into Resolume on macOS.** Everything numeric is measured offline on
  macOS, plus an `oxbow` load. Footage seen only through `--pipe` (Resolume's demo
  clips, the project video), judged by eye.
- On Windows the v0.1.0 DLL ran in Arena 7.27.1 on win-lab (software rendering) through
  the fleet gate: 9/9, all 18 controls moving; the verdict is in AGENTS.md and the README.
- No OpenFX port, no factory presets, no audio input.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are GENERATED by the backend's sync scripts;
  do not hand-edit them. The user guide is `docs/USER-GUIDE.md`, rendered to the site and
  `docs/USER-GUIDE.pdf` by the website's `build_guides.py`.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/toner/toner.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\toner\logs\toner.YYYY-MM-DD.log   (Windows)
