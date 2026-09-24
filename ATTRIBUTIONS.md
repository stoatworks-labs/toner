# Attributions

Toner is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Harness shape, --pipe contract and verify — Stoatworks slope and clamp

<https://github.com/stoatworks-labs/clamp>  
Licence: MIT  
Copyright: Stoatworks Labs

The harness shape, the --pipe contract (SIGPIPE ignored, a closed stdout exits 1), the negative-control pattern, --offline, check-shaders.sh, the verify script, the sweep and the CI workflows are clamp's, by way of slope.

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

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Xerographic imaging

The photo-induced discharge curve, the fringe-field (edge effect) account of why wide solids develop hollow and thin lines full, the developer electrode as the remedy, and toner starvation along the process direction are the standard account of electrophotography as given in the textbooks (Schaffert, Electrophotography; Pai and Springett, "Physics of electrophotography", Rev. Mod. Phys. 65, 1993). Nothing was copied from them; the strip-over-a-ground-plane field is the image-charge result from any electrostatics text, and its Cauchy kernel form and the separable approximation here are this repo's.

## Standards and published specifications

What the implementation is measured against.

- **ITU-R BT.601** — The luma weights (0.299, 0.587, 0.114) the page is read from the source with.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
