#pragma once

/**
	The passes. Every read is `texelFetch` at integer coordinates computed
	in integers, so nothing here depends on a texture unit's filtering or on
	where a rasteriser's interpolated uv lands (the one exception is the
	composite's read of the host's picture, which maps the host viewport onto
	it). Every coefficient -- the blur weights, the two field kernels, the
	development window, the placement matrix, the defect table -- is computed
	on the CPU in double (`Controls.cpp`, `Toner.cpp`) and handed over as a
	float uniform; the GPU multiplies, adds, compares and takes one `exp` per
	pixel for the discharge curve and one per defect for the drum.

	All page buffers are in GL's own orientation (row 0 at the bottom), the
	same as the host's picture, so no pass flips anything. "Down the page"
	is the process direction option, which maps rows to a process index.

	Per generation, in order:

	  place      page in   -> placed: this generation's skew, offset and zoom
	  blur (x2)  placed    -> blurred: a separable Gaussian of Optics pixels
	  ae         blurred   -> 1 texel: the AE sensor's reading B
	  latent     blurred+B -> q: auto exposure, the halftone screen in Photo
	                          Mode, the discharge curve
	  fieldx     q         -> ( C_h * q, C_H * q ) along x
	  develop    field+q   -> D: the y pass of both kernels, the difference,
	                          the gain, the electrode, the window
	  coverage   D         -> L x 1: each process row's mean density
	  supply     coverage  -> L x 1: the developer's concentration per row
	  marks      D+supply  -> D': starvation applied, the drum's defects
	  fuse       D'        -> page out: the toner spread; 1 - density

	  intake     the host's picture -> page 0 (luma), once a frame
	  composite  the last page -> the host: paper, toner, mix
*/
namespace tonerfx::shaders
{

extern const char* const kVertex;
extern const char* const kIntake;
extern const char* const kPlace;
extern const char* const kBlur;
extern const char* const kAE;
extern const char* const kLatent;
extern const char* const kFieldX;
extern const char* const kDevelop;
extern const char* const kCoverage;
extern const char* const kSupply;
extern const char* const kMarks;
extern const char* const kFuse;
extern const char* const kComposite;

/// How many fragment shaders there are, for the dump and the check.
constexpr int kFragmentCount = 12;

} // namespace tonerfx::shaders
