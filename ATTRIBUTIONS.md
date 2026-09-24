# Attributions

Toner is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

This is a PROVISIONAL hand copy (2026-09-24). The real file is generated — the
master lists live in the `stoatworks-backend` repo and are pushed out by
`scripts/sync-attributions.py` once the project is registered. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Harness shape, --pipe contract and verify — Stoatworks slope and clamp

<https://github.com/stoatworks-labs/clamp>  
Licence: MIT  
Copyright: Stoatworks Labs

The harness shape, the --pipe contract (SIGPIPE ignored), the negative-control pattern, --offline, check-shaders.sh, the verify script, the sweep and the CI workflows are clamp's, by way of slope.

### The halftone as a spot function — Stoatworks rosette

<https://github.com/stoatworks-labs/rosette>  
Licence: MIT  
Copyright: Stoatworks Labs

The idea of a screen as a spot function whose threshold is chosen so the dot's area is exactly the tone is rosette's; the diamond dot with its closed-form area law is this repo's own.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

The off-screen buffer wrapper is tinsel's, by way of slope.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### zlib

<https://zlib.net>  
Licence: zlib  
Copyright: Jean-loup Gailly and Mark Adler

The system's copy, linked by the offline harness only, for its PNG writer. Not shipped in the bundle.

## Reference

Nothing was copied from these; they are what the model was built from.

### Xerographic imaging

The photo-induced discharge curve, the fringe-field ("edge effect") account of why wide solids develop hollow and thin lines full, the developer electrode as the remedy, and toner starvation along the process direction are the standard account of electrophotography as given in the textbooks (Schaffert, *Electrophotography*; Pai and Springett, "Physics of electrophotography", *Rev. Mod. Phys.* 65, 1993). The strip-over-a-ground-plane field is the image-charge result from any electrostatics text; its Cauchy (Poisson) kernel form and the separable approximation here are this repo's.
