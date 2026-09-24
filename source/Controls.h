#pragma once

/**
	What a host parameter means.

	Every ranged host parameter is 0..1 (SetParamInfo clamps a STANDARD
	default into 0..1 before a range can be attached), and an option
	parameter's range reads back 0..1 whatever its element count -- so an
	option is its element INDEX, rounded and clamped here, never a fraction
	of a range. Generations is a real FF_TYPE_INTEGER with a real range.
	Every conversion to a physical unit lives here and nowhere else; totest
	states the same laws from their definitions and --laws holds the two
	together.
*/
namespace tonerfx::controls
{

/// An option's stored value, as an index into its `count` elements.
int OptionIndex( float value, int count );

/// Generations: the integer, clamped to 1..8.
int Generations( float value );

/// Contrast: the tone curve's slope at mid-grey, 1.6 x 10^v -- 1.6 at 0,
/// about 5.1 at the default 0.5, 16 at 1. 1.6 is just above the slope at
/// which the development window's lower edge reaches zero charge
/// (model::MinSlope(), 1.58).
double ContrastSlope( float value );

/// Bg Suppression: how far the AE reading is trusted, 0 (the page is
/// printed as it is) to 1 (the reading is paper white). The divisor is
/// mix( 1, B, v ).
double BgSuppression( float value );

/// Optics: the Gaussian's sigma in pixels, 4 v^2 -- none at 0, one pixel at
/// the default 0.5, four at 1.
double OpticsSigma( float value );

/// Zoom: the scale per generation, 1 + 0.08 ( v - 0.5 ): 96% at 0, exactly
/// 100% at 0.5, 104% at 1.
double Zoom( float value );

/// Skew: the largest rotation per generation, in degrees, 2 v.
double SkewDegrees( float value );

/// The largest offset per generation, as a fraction of the page's shorter
/// side: half a percent at Skew 1.
double OffsetFraction( float value );

/// Solid Fill: the developer electrode's weight, v.
double SolidFill( float value );

/// Toner Supply: the depletion per unit coverage over a whole page,
/// 6 ( 1 - v )^2 -- six at 0, none at 1 (an unlimited supply), 0.54 at the
/// default 0.7.
double DepletionPerPage( float value );

/// Recovery: the developer's recovery over a whole page, 6 v^2.
double RecoveryPerPage( float value );

/// Screen: the halftone cell's pitch in pixels, 4 x 2^( 2v ): 4 to 16.
double ScreenPitchPx( float value );

/// Screen Angle: 180 v degrees.
double ScreenAngleDegrees( float value );

/// Drum Marks: the defects' amplitude, v.
double DrumMarks( float value );

/// Circumference: the drum's circumference as a fraction of the page along
/// the process direction, 0.1 + 0.9 v.
double CircumferenceFraction( float value );

/// The per-row starvation rates for a page of `rows` along the process
/// direction: the per-page rate over the rows.
double PerRow( double perPage, int rows );

} // namespace tonerfx::controls
