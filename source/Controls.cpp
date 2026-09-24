#include "Controls.h"

#include "Model.h"

#include <algorithm>
#include <cmath>

namespace tonerfx::controls
{
namespace
{
double unit( float value )
{
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 );
}
} // namespace

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

int Generations( float value )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), model::kGenerationsMin, model::kGenerationsMax );
}

double ContrastSlope( float value )
{
	return 1.6 * std::pow( 10.0, unit( value ) );
}

double BgSuppression( float value )
{
	return unit( value );
}

double OpticsSigma( float value )
{
	const double v = unit( value );
	return 4.0 * v * v;
}

double Zoom( float value )
{
	return 1.0 + 0.08 * ( unit( value ) - 0.5 );
}

double SkewDegrees( float value )
{
	return 2.0 * unit( value );
}

double OffsetFraction( float value )
{
	return 0.005 * unit( value );
}

double SolidFill( float value )
{
	return unit( value );
}

double DepletionPerPage( float value )
{
	const double v = 1.0 - unit( value );
	return 6.0 * v * v;
}

double RecoveryPerPage( float value )
{
	const double v = unit( value );
	return 6.0 * v * v;
}

double ScreenPitchPx( float value )
{
	return 4.0 * std::exp2( 2.0 * unit( value ) );
}

double ScreenAngleDegrees( float value )
{
	return 180.0 * unit( value );
}

double DrumMarks( float value )
{
	return unit( value );
}

double CircumferenceFraction( float value )
{
	return 0.1 + 0.9 * unit( value );
}

double PerRow( double perPage, int rows )
{
	return rows > 0 ? perPage / rows : 0.0;
}

} // namespace tonerfx::controls
