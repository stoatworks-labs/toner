#pragma once

#include <cmath>
#include <cstdint>

/**
	The copier as numbers: what the shaders compute, written down once, with
	no GL in it. The plugin's C++ uses the constants, the option tables, the
	hash and the seeded tables (skew per generation, the drum's defects); the
	pixels themselves are made in GLSL (`Shaders.cpp`) and the harness
	restates the model from this description and holds the shaders to it.

	**The page is the frame, and a copy is the whole machine run once.** N
	generations a frame: generation g takes generation g - 1's page (the
	source's luma for g = 1) through, in order,

	  place       zoom Z, this generation's skew theta_g and offset d_g about
	              the page centre; bilinear; off the page is paper (1)
	  optics      a Gaussian of sigma = Optics pixels, separable
	  auto exp.   B = the mean of the brightest sixteenth of a 64 x 36 grid
	              of point samples (the AE sensor reads the background); the
	              page is scaled so that mix( 1, B, Bg Suppression ) is paper
	  screen      Photo Mode only: a diamond-dot halftone at Screen pixels and
	              Screen Angle, the dot's area exactly the tone (closed form)
	  photocond.  the latent charge q( R ) = ( e^( -k R ) - e^-k ) / ( 1 - e^-k ),
	              k = kPidc: the photo-induced discharge curve, 1 at black,
	              0 at paper
	  develop     toner follows the FIELD above the latent image, not the
	              charge: E = q * ( C_h x C_h ) - q * ( C_H x C_H ), the
	              difference of a Cauchy (Poisson) kernel at the development
	              gap h and one at h + 2t (the image charge below the
	              grounded photoconductor of thickness t). A wide charged
	              area's field is zero at its centre and peaks near its
	              edges; a thin line's is proportional to its width. Gain
	              G = 1 / E( a one-pixel line at full charge ), so a
	              one-pixel line develops as a solid does under the
	              electrode. A developer electrode
	              (Solid Fill s) flattens the field toward the parallel-plate
	              value, which is q itself: E' = ( 1 - s ) G E + s q. Toner:
	              D = clamp( ( E' - lo ) / ( hi - lo ), 0, 1 ), with the
	              development window lo..hi centred on q( kMidGrey ) and
	              its width set by Contrast (the slope of the tone curve at
	              mid-grey), so that with the electrode in, one generation is
	              the S-curve T( R ) = 1 - D( q( R ) ) with stable fixed
	              points at 0 and 1 and an unstable one at kMidGrey
	  starvation  the developer's toner concentration c along the process
	              direction, one row at a time: c_0 = 1,
	              c_{j+1} = c_j + r ( 1 - c_j ) - kappa a_j c_j, where a_j is
	              row j's mean developed density, r = Recovery / L and
	              kappa = depletion / L per row over L rows. On a page of
	              constant coverage a that is c_j = c_inf + ( 1 - c_inf )
	              ( 1 - lambda )^j, lambda = r + kappa a, c_inf = r / lambda.
	              The density laid on row j is D c_j
	  drum        kDrumDefects seeded defects per circumference C (a fraction
	              of L), each a Gaussian speck (added) or dropout
	              (multiplied), repeating at u0 + m C down the process axis
	  fuse        the toner spreads by a separable 3-tap [ s, 1 - 2s, s ],
	              s = kFuseSpread; the page out is 1 - density

	and the last generation's page is toner black on the paper stock, mixed
	with the input.

	**Everything random is an integer hash** (a PCG output mix, exact in 32
	bits, the fleet's `hashInt`), seeded by generation or defect index and
	nothing else, so a frame is the same picture every time it is rendered
	and on every machine.
*/
namespace tonerfx::model
{

/// Negative-control hooks: a bitmask the shipped plugin always carries at 0.
/// Each one perturbs the PLUGIN's shaders or its uniforms, never the
/// harness's expectation.
enum Perturb : int
{
	kPerturbNone         = 0,
	kPerturbNoFringe     = 1 << 0,///< the field kernel replaced by a plain threshold on the charge (E' = q)
	kPerturbSameSkew     = 1 << 1,///< every generation reuses generation 1's skew and offset
	kPerturbAEMean       = 1 << 2,///< the AE sensor reads the page mean instead of the brightest sixteenth
	kPerturbStarveDemand = 1 << 3,///< the developer depletes by the demanded coverage, not the laid one (no c factor)
	kPerturbDrumDrift    = 1 << 4,///< the drum's repeats are spaced 1.01 circumferences apart
	kPerturbLinearTone   = 1 << 5,///< the photoconductor is linear: q = 1 - R, and the window is 0..1
};

constexpr int kGenerationsMin = 1;
constexpr int kGenerationsMax = 8;

//---------------------------------------------------------------------------
// The photoconductor and the development window.
//---------------------------------------------------------------------------

/// The discharge curve's exponent: exposure at paper white discharges the
/// drum to e^-2 of its dark voltage before the residual is subtracted.
constexpr double kPidc = 2.0;

/// The unstable fixed point of the tone curve: the grey that copies as
/// itself. Everything lighter goes to paper, everything darker to toner.
constexpr double kMidGrey = 0.5;

/// The latent charge for a reflectance R, 1 at black and 0 at paper.
inline double Charge( double R )
{
	const double floor_ = std::exp( -kPidc );
	return ( std::exp( -kPidc * R ) - floor_ ) / ( 1.0 - floor_ );
}

/// dq/dR at mid-grey, negated: the charge falls this fast per unit
/// reflectance where the curve is centred.
inline double ChargeSlopeAtMid()
{
	return kPidc * std::exp( -kPidc * kMidGrey ) / ( 1.0 - std::exp( -kPidc ) );
}

/// The development window for a tone-curve slope S at mid-grey: toner
/// starts at charge `lo` and is solid at `hi`, centred on q( kMidGrey ),
/// width ChargeSlopeAtMid() / S. Contrast's law (Controls.h) keeps S high
/// enough that lo >= 0.
inline void DevelopmentWindow( double slope, double& lo, double& hi )
{
	const double mid   = Charge( kMidGrey );
	const double width = ChargeSlopeAtMid() / slope;
	lo                 = mid - width / 2.0;
	hi                 = mid + width / 2.0;
}

/// The lowest slope the window allows: below it the window's lower edge
/// would fall under zero charge. Contrast's law stays above it.
inline double MinSlope()
{
	return ChargeSlopeAtMid() / ( 2.0 * Charge( kMidGrey ) );
}

//---------------------------------------------------------------------------
// The field.
//---------------------------------------------------------------------------

/// The development gap and the photoconductor's thickness, in pixels. The
/// image charge sits 2t below the surface, so the two kernels are at h and
/// h + 2t. Half a pixel each: a 20 um photoconductor under a 300 mm page is
/// a fifteenth of a pixel at 1080 rows, so even this is generous, and it is
/// what makes a 10-pixel stroke solid and a 40-pixel one hollow.
constexpr double kGapPx       = 0.5;
constexpr double kThicknessPx = 0.5;

/// Taps either side of centre in each 1-D pass: 2 x 16 + 1 = 33 taps. The
/// Cauchy tail beyond it (6% of the wide kernel's mass) is dropped and the
/// kernel renormalised, so a solid wider than 33 pixels has exactly no
/// field at points more than 16 pixels from its edge. The field shaders
/// declare their arrays at this size.
constexpr int kKernelRadius = 16;

/// One texel of the 1-D Cauchy kernel at scale `h`, the exact integral of
/// h / ( pi ( x^2 + h^2 ) ) over texel offset j, before normalisation.
inline double CauchyTexel( int j, double h )
{
	return ( std::atan( ( j + 0.5 ) / h ) - std::atan( ( j - 0.5 ) / h ) ) / M_PI;
}

/// The mass of the truncated kernel, which it is normalised by.
inline double CauchyMass( double h )
{
	return 2.0 * std::atan( ( kKernelRadius + 0.5 ) / h ) / M_PI;
}

/// The normalised texel: what the shader multiplies by.
inline double KernelTexel( int j, double h )
{
	return CauchyTexel( j, h ) / CauchyMass( h );
}

/// The development gain: one over the field of a one-pixel line (long
/// along y, so its y factor is the full kernel, 1) at full charge. So the
/// field is in units of the parallel-plate field of a full-charge solid,
/// and a one-pixel line develops exactly as such a solid does under the
/// electrode: well past the window's top, solid.
inline double FieldGain()
{
	const double H = kGapPx + 2.0 * kThicknessPx;
	return 1.0 / ( KernelTexel( 0, kGapPx ) - KernelTexel( 0, H ) );
}

//---------------------------------------------------------------------------
// The rest of the machine.
//---------------------------------------------------------------------------

/// Fuse: the toner spreads by this much into each neighbour, per axis.
constexpr double kFuseSpread = 0.15;

/// Auto exposure: a 64 x 36 grid of point samples, 64 bins, the brightest
/// sixteenth averaged; the reading is floored so a black page cannot divide
/// by nothing.
constexpr int kAEColumns     = 64;
constexpr int kAERows        = 36;
constexpr int kAEBins        = 64;
constexpr double kAEFraction = 1.0 / 16.0;
constexpr double kAEFloor    = 1.0 / 32.0;

/// The drum: eight defects per circumference, the first five toner specks
/// and the rest dropouts, each a Gaussian of 1 to 3 pixels.
constexpr int kDrumDefects  = 8;
constexpr int kDrumSpecks   = 5;
constexpr double kDrumSigmaMin = 1.0;
constexpr double kDrumSigmaMax = 3.0;

/// Seeds. Arbitrary and fixed: change one and every composition's drum
/// marks move.
constexpr uint32_t kSkewSeed = 0x544F4E01u;//'TON', 1
constexpr uint32_t kDrumSeed = 0x544F4E02u;

//---------------------------------------------------------------------------
// Options.
//---------------------------------------------------------------------------

enum Paper
{
	kWhite = 0,
	kCream,
	kGoldenrod,
	kPink,
	kBlue,
	kGreen,
	kNewsprint,
	kPaperCount
};
inline const char* const kPaperNames[ kPaperCount ] = { "White", "Cream", "Goldenrod", "Pink", "Blue", "Green", "Newsprint" };
inline const float kPaperRGB[ kPaperCount ][ 3 ]     = {
    { 1.0f, 1.0f, 1.0f }, { 1.0f, 0.97f, 0.88f }, { 1.0f, 0.82f, 0.35f }, { 1.0f, 0.80f, 0.85f },
    { 0.75f, 0.85f, 1.0f }, { 0.78f, 0.95f, 0.80f }, { 0.85f, 0.83f, 0.78f },
};

/// The process direction: which way the paper moves under the drum, so
/// which way starvation deepens and the drum's repeats run.
enum Direction
{
	kDown = 0,
	kUp,
	kRight,
	kLeft,
	kDirectionCount
};
inline const char* const kDirectionNames[ kDirectionCount ] = { "Down", "Up", "Right", "Left" };

//---------------------------------------------------------------------------
// The hash and the seeded tables.
//---------------------------------------------------------------------------

/// The fleet's PCG output mix. Exact in 32 bits, identical everywhere.
inline uint32_t HashInt( uint32_t v )
{
	uint32_t state = v * 747796405u + 2891336453u;
	uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

/// The top 24 bits as 0..1: exact in a float.
inline double Unit( uint32_t h )
{
	return ( h >> 8 ) * ( 1.0 / 16777216.0 );
}

/// -1..1.
inline double Signed( uint32_t h )
{
	return Unit( h ) * 2.0 - 1.0;
}

/// Generation g's (1-based) skew as a fraction of the maximum, and its
/// offset as fractions of the offset amplitude, each in -1..1.
inline double SkewFraction( int g )
{
	return Signed( HashInt( kSkewSeed + 3u * static_cast< uint32_t >( g ) ) );
}
inline double OffsetXFraction( int g )
{
	return Signed( HashInt( kSkewSeed + 3u * static_cast< uint32_t >( g ) + 1u ) );
}
inline double OffsetYFraction( int g )
{
	return Signed( HashInt( kSkewSeed + 3u * static_cast< uint32_t >( g ) + 2u ) );
}

/// Defect i's position round the circumference and across the page, both as
/// fractions, its sigma in pixels, and whether it is a speck.
struct Defect
{
	double u;///< fraction of a circumference
	double v;///< fraction of the page across the process direction
	double sigma;
	bool speck;
};
inline Defect DrumDefect( int i )
{
	const uint32_t base = kDrumSeed + 4u * static_cast< uint32_t >( i );
	Defect d;
	d.u     = Unit( HashInt( base ) );
	d.v     = Unit( HashInt( base + 1u ) );
	d.sigma = kDrumSigmaMin + ( kDrumSigmaMax - kDrumSigmaMin ) * Unit( HashInt( base + 2u ) );
	d.speck = i < kDrumSpecks;
	return d;
}

} // namespace tonerfx::model
