#include "Shaders.h"

#include <string>

namespace tonerfx::shaders
{

const char* const kVertex = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//---------------------------------------------------------------------------
// The process direction, shared by coverage, supply and marks. A page pixel
// ( x, y ) in GL coordinates has a process index u (0 where the paper enters
// the machine) and an across index v; L is the page's length along the
// process direction and A its width across it.
//---------------------------------------------------------------------------
static const char* const kProcessLib = R"(
uniform int Direction;  //0 down, 1 up, 2 right, 3 left
uniform int PageW;
uniform int PageH;

ivec2 processOf( ivec2 p )
{
	if( Direction == 0 )
		return ivec2( PageH - 1 - p.y, p.x );
	if( Direction == 1 )
		return ivec2( p.y, p.x );
	if( Direction == 2 )
		return ivec2( p.x, p.y );
	return ivec2( PageW - 1 - p.x, p.y );
}

ivec2 pixelOf( int u, int v )
{
	if( Direction == 0 )
		return ivec2( v, PageH - 1 - u );
	if( Direction == 1 )
		return ivec2( v, u );
	if( Direction == 2 )
		return ivec2( u, v );
	return ivec2( PageW - 1 - u, v );
}

int processLength()
{
	return Direction < 2 ? PageH : PageW;
}

int acrossLength()
{
	return Direction < 2 ? PageW : PageH;
}
)";

//---------------------------------------------------------------------------
// intake: the host's picture to page 0, as luma.
//---------------------------------------------------------------------------
const char* const kIntake = R"(#version 410 core

uniform sampler2D InputTexture;

out vec4 fragColor;

const vec3 kLuma = vec3( 0.299, 0.587, 0.114 );

void main()
{
	ivec2 p   = ivec2( gl_FragCoord.xy );
	vec4 c    = texelFetch( InputTexture, p, 0 );
	fragColor = vec4( dot( c.rgb, kLuma ), 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// place: the page under the platen, this generation's way.
//
// The source point of output pixel centre p is s = Inv ( p - Centre -
// Offset ) + Centre, with Inv the inverse of zoom x rotation, computed on
// the CPU in double. Bilinear by hand from four texelFetches: with an
// identity placement s - 0.5 is the integer pixel exactly, the fraction is
// exactly 0, and the page passes through untouched -- which is what lets
// the tone-curve checks see the curve and nothing else. Off the page is
// paper.
//---------------------------------------------------------------------------
const char* const kPlace = R"(#version 410 core

uniform sampler2D PageTexture;
uniform int PageW;
uniform int PageH;
uniform vec4 Inv;     //( a, b, c, d ): s = ( a x + b y, c x + d y )
uniform vec2 Centre;
uniform vec2 Offset;

out vec4 fragColor;

float pageAt( int x, int y )
{
	return texelFetch( PageTexture, ivec2( clamp( x, 0, PageW - 1 ), clamp( y, 0, PageH - 1 ) ), 0 ).r;
}

void main()
{
	vec2 p = gl_FragCoord.xy;//pixel centre
	vec2 r = p - Centre - Offset;
	vec2 s = vec2( Inv.x * r.x + Inv.y * r.y, Inv.z * r.x + Inv.w * r.y ) + Centre;

	if( s.x < 0.0 || s.y < 0.0 || s.x > float( PageW ) || s.y > float( PageH ) )
	{
		fragColor = vec4( 1.0, 0.0, 0.0, 1.0 );
		return;
	}

	vec2 t  = s - 0.5;
	vec2 b  = floor( t );
	vec2 f  = t - b;
	int x0  = int( b.x );
	int y0  = int( b.y );
	float v00 = pageAt( x0, y0 );
	float v10 = pageAt( x0 + 1, y0 );
	float v01 = pageAt( x0, y0 + 1 );
	float v11 = pageAt( x0 + 1, y0 + 1 );
	float v0  = v00 * ( 1.0 - f.x ) + v10 * f.x;
	float v1  = v01 * ( 1.0 - f.x ) + v11 * f.x;
	fragColor = vec4( v0 * ( 1.0 - f.y ) + v1 * f.y, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// blur: one axis of the optics' Gaussian. Weights[ 0 ] is the centre;
// Radius 0 with Weights[ 0 ] = 1 is the identity, exactly.
//---------------------------------------------------------------------------
const char* const kBlur = R"(#version 410 core

uniform sampler2D PageTexture;
uniform int PageW;
uniform int PageH;
uniform int Axis;          //0 along x, 1 along y
uniform int Radius;        //taps either side
uniform float Weights[ 13 ];

out vec4 fragColor;

float pageAt( ivec2 p )
{
	return texelFetch( PageTexture, clamp( p, ivec2( 0 ), ivec2( PageW - 1, PageH - 1 ) ), 0 ).r;
}

void main()
{
	ivec2 p    = ivec2( gl_FragCoord.xy );
	ivec2 step = Axis == 0 ? ivec2( 1, 0 ) : ivec2( 0, 1 );
	float sum  = Weights[ 0 ] * pageAt( p );
	for( int j = 1; j <= Radius; ++j )
		sum += Weights[ j ] * ( pageAt( p + j * step ) + pageAt( p - j * step ) );
	fragColor = vec4( sum, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// ae: the AE sensor. One texel. A 64 x 36 grid of point samples of the
// blurred page, 64 bins, and the mean of the brightest sixteenth of them,
// the crossing bin taken at its own mean. Floored at AEFloor.
//---------------------------------------------------------------------------
const char* const kAE = R"(#version 410 core

uniform sampler2D PageTexture;
uniform int PageW;
uniform int PageH;
uniform float AEFloor;
uniform int Perturb;

out vec4 fragColor;

const int kColumns = 64;
const int kRows    = 36;
const int kBins    = 64;

void main()
{
	float count[ kBins ];
	float sum[ kBins ];
	for( int b = 0; b < kBins; ++b )
	{
		count[ b ] = 0.0;
		sum[ b ]   = 0.0;
	}
	float total    = 0.0;
	float totalSum = 0.0;
	for( int j = 0; j < kRows; ++j )
	{
		int y = ( ( 2 * j + 1 ) * PageH ) / ( 2 * kRows );
		for( int i = 0; i < kColumns; ++i )
		{
			int x   = ( ( 2 * i + 1 ) * PageW ) / ( 2 * kColumns );
			float s = clamp( texelFetch( PageTexture, ivec2( x, y ), 0 ).r, 0.0, 1.0 );
			int b   = min( kBins - 1, int( s * float( kBins ) ) );
			count[ b ] += 1.0;
			sum[ b ] += s;
			total += 1.0;
			totalSum += s;
		}
	}

	float reading;
	if( ( Perturb & 4 ) != 0 )
	{
		//Perturb 4: the sensor reads the page mean (a negative control).
		reading = totalSum / total;
	}
	else
	{
		float need = total / 16.0;
		float have = 0.0;
		float acc  = 0.0;
		for( int b = kBins - 1; b >= 0; --b )
		{
			if( count[ b ] <= 0.0 )
				continue;
			if( have + count[ b ] <= need )
			{
				have += count[ b ];
				acc += sum[ b ];
				continue;
			}
			float f = ( need - have ) / count[ b ];
			acc += f * sum[ b ];
			have = need;
			break;
		}
		reading = acc / need;
	}
	fragColor = vec4( max( reading, AEFloor ), 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// latent: auto exposure, the screen, the photoconductor.
//
// The reflectance is scaled so that mix( 1, B, Suppression ) is paper, then
// in Photo Mode screened with diamond dots whose area is exactly the tone
// (a diamond |x| + |y| <= 2T in the cell [-1,1]^2 covers 2T^2 of it for
// T <= 1/2 and 1 - 2( 1 - T )^2 above), then discharged: q = ( e^-kR -
// e^-k ) / ( 1 - e^-k ).
//---------------------------------------------------------------------------
const char* const kLatent = R"(#version 410 core

uniform sampler2D PageTexture;
uniform sampler2D AETexture;
uniform float Suppression;
uniform int PhotoMode;
uniform float ScreenPitch;    //pixels per cell
uniform vec2 ScreenAxis;      //( cos, sin ) of the screen angle
uniform float Pidc;           //k
uniform float PidcFloor;      //e^-k
uniform int Perturb;

out vec4 fragColor;

float screened( float R, vec2 p )
{
	float a = clamp( 1.0 - R, 0.0, 1.0 );//the tone as coverage
	float T = a <= 0.5 ? sqrt( a * 0.5 ) : 1.0 - sqrt( ( 1.0 - a ) * 0.5 );

	//Into the screen's frame: cells of ScreenPitch pixels along ScreenAxis.
	vec2 q    = vec2( ScreenAxis.x * p.x + ScreenAxis.y * p.y, -ScreenAxis.y * p.x + ScreenAxis.x * p.y ) / ScreenPitch;
	vec2 cell = q - floor( q );
	vec2 c    = 2.0 * cell - 1.0;
	float spot = 0.5 * ( abs( c.x ) + abs( c.y ) );

	//A three-quarter-pixel edge in spot units.
	float w   = 0.75 / ScreenPitch;
	float ink = 1.0 - smoothstep( T - w, T + w, spot );
	ink       = ink * smoothstep( 0.0, 0.02, a );
	ink       = max( ink, smoothstep( 0.98, 1.0, a ) );
	return 1.0 - ink;
}

void main()
{
	ivec2 p  = ivec2( gl_FragCoord.xy );
	float R  = texelFetch( PageTexture, p, 0 ).r;
	float B  = texelFetch( AETexture, ivec2( 0, 0 ), 0 ).r;
	float Bm = mix( 1.0, B, Suppression );
	float Rn = min( 1.0, R / Bm );

	if( PhotoMode == 1 )
		Rn = screened( Rn, gl_FragCoord.xy );

	float q;
	if( ( Perturb & 32 ) != 0 )
		q = 1.0 - Rn;//Perturb 32: a linear photoconductor (a negative control)
	else
		q = clamp( ( exp( -Pidc * Rn ) - PidcFloor ) / ( 1.0 - PidcFloor ), 0.0, 1.0 );
	fragColor = vec4( q, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// fieldx: the x pass of both field kernels. KernelH is the Cauchy at the
// development gap, KernelHH the one at gap + 2 x thickness (the image
// charge); index Radius is the centre. Reads clamp to the page edge, which
// is the charge continuing past the edge -- the platen's edge is not a
// charge edge.
//---------------------------------------------------------------------------
const char* const kFieldX = R"(#version 410 core

uniform sampler2D ChargeTexture;
uniform int PageW;
uniform int PageH;
uniform int Radius;
uniform float KernelH[ 33 ];
uniform float KernelHH[ 33 ];

out vec4 fragColor;

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	float near = 0.0;
	float far  = 0.0;
	for( int j = -Radius; j <= Radius; ++j )
	{
		int x   = clamp( p.x + j, 0, PageW - 1 );
		float q = texelFetch( ChargeTexture, ivec2( x, p.y ), 0 ).r;
		near += KernelH[ j + Radius ] * q;
		far += KernelHH[ j + Radius ] * q;
	}
	fragColor = vec4( near, far, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// develop: the y pass, the difference, the gain, the electrode, the window.
//---------------------------------------------------------------------------
const char* const kDevelop = R"(#version 410 core

uniform sampler2D FieldTexture;
uniform sampler2D ChargeTexture;
uniform int PageW;
uniform int PageH;
uniform int Radius;
uniform float KernelH[ 33 ];
uniform float KernelHH[ 33 ];
uniform float Gain;
uniform float Electrode;     //Solid Fill
uniform float WindowLo;
uniform float WindowHi;
uniform int Perturb;

out vec4 fragColor;

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	float near = 0.0;
	float far  = 0.0;
	for( int j = -Radius; j <= Radius; ++j )
	{
		int y   = clamp( p.y + j, 0, PageH - 1 );
		vec2 f  = texelFetch( FieldTexture, ivec2( p.x, y ), 0 ).rg;
		near += KernelH[ j + Radius ] * f.r;
		far += KernelHH[ j + Radius ] * f.g;
	}
	float field  = Gain * ( near - far );
	float charge = texelFetch( ChargeTexture, p, 0 ).r;

	float e;
	if( ( Perturb & 1 ) != 0 )
		e = charge;//Perturb 1: a plain threshold on the charge (a negative control)
	else
		e = ( 1.0 - Electrode ) * field + Electrode * charge;

	float density;
	if( ( Perturb & 32 ) != 0 )
		density = clamp( e, 0.0, 1.0 );//Perturb 32: the window is 0..1
	else
		density = clamp( ( e - WindowLo ) / ( WindowHi - WindowLo ), 0.0, 1.0 );
	fragColor = vec4( density, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// coverage: each process row's mean developed density, into texel u of an
// L x 1 buffer.
//---------------------------------------------------------------------------
static const char* const kCoverageMain = R"(
uniform sampler2D DensityTexture;

out vec4 fragColor;

void main()
{
	int u = int( gl_FragCoord.x );
	int n = acrossLength();
	float sum = 0.0;
	for( int v = 0; v < n; ++v )
		sum += texelFetch( DensityTexture, pixelOf( u, v ), 0 ).r;
	fragColor = vec4( sum / float( n ), 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// supply: the developer's toner concentration at each process row, the
// recurrence run from the page's leading edge. Row u is developed with
// c_u, before its own depletion.
//---------------------------------------------------------------------------
static const char* const kSupplyMain = R"(
uniform sampler2D CoverageTexture;
uniform float Recovery;     //per row
uniform float Depletion;    //per row per unit coverage
uniform int Perturb;

out vec4 fragColor;

void main()
{
	int u   = int( gl_FragCoord.x );
	float c = 1.0;
	for( int j = 0; j < u; ++j )
	{
		float a = texelFetch( CoverageTexture, ivec2( j, 0 ), 0 ).r;
		if( ( Perturb & 8 ) != 0 )
			c = c + Recovery * ( 1.0 - c ) - Depletion * a;//Perturb 8: depletes by the demand (a negative control)
		else
			c = c + Recovery * ( 1.0 - c ) - Depletion * a * c;
	}
	fragColor = vec4( c, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// marks: starvation applied, then the drum's defects. Each defect sits at
// ( DefectU + m Circumference, DefectV ) in process coordinates for every
// whole m >= 0; only the nearest repeat can reach a pixel, the sigmas being
// far smaller than a circumference. Dropouts multiply the toner, specks add.
//---------------------------------------------------------------------------
static const char* const kMarksMain = R"(
uniform sampler2D DensityTexture;
uniform sampler2D SupplyTexture;
uniform float Circumference;   //pixels along the process direction
uniform float Amplitude;
uniform float DefectU[ 8 ];
uniform float DefectV[ 8 ];
uniform float DefectSigma[ 8 ];
uniform int DefectSpeck[ 8 ];
uniform int Perturb;

out vec4 fragColor;

void main()
{
	ivec2 p   = ivec2( gl_FragCoord.xy );
	ivec2 uv2 = processOf( p );
	float d   = texelFetch( DensityTexture, p, 0 ).r;
	float c   = texelFetch( SupplyTexture, ivec2( uv2.x, 0 ), 0 ).r;
	d *= c;

	if( Amplitude > 0.0 )
	{
		vec2 pc   = vec2( uv2 ) + 0.5;
		float spacing = ( ( Perturb & 16 ) != 0 ) ? Circumference * 1.01 : Circumference;//Perturb 16: the repeats drift (a negative control)
		float keep = 1.0;
		float add  = 0.0;
		for( int i = 0; i < 8; ++i )
		{
			float m      = max( 0.0, floor( ( pc.x - DefectU[ i ] ) / spacing + 0.5 ) );
			float du     = pc.x - ( DefectU[ i ] + m * spacing );
			float dv     = pc.y - DefectV[ i ];
			float g      = exp( -( du * du + dv * dv ) / ( 2.0 * DefectSigma[ i ] * DefectSigma[ i ] ) );
			if( DefectSpeck[ i ] == 1 )
				add += Amplitude * g;
			else
				keep *= 1.0 - Amplitude * g;
		}
		d = d * keep + add;
	}
	fragColor = vec4( clamp( d, 0.0, 1.0 ), 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// fuse: the toner spreads into its neighbours; the page out is reflectance.
//---------------------------------------------------------------------------
const char* const kFuse = R"(#version 410 core

uniform sampler2D DensityTexture;
uniform int PageW;
uniform int PageH;
uniform float Spread;

out vec4 fragColor;

float at( ivec2 p )
{
	return texelFetch( DensityTexture, clamp( p, ivec2( 0 ), ivec2( PageW - 1, PageH - 1 ) ), 0 ).r;
}

void main()
{
	ivec2 p  = ivec2( gl_FragCoord.xy );
	float w0 = 1.0 - 2.0 * Spread;
	float w1 = Spread;
	float row0 = w1 * at( p + ivec2( -1, -1 ) ) + w0 * at( p + ivec2( 0, -1 ) ) + w1 * at( p + ivec2( 1, -1 ) );
	float row1 = w1 * at( p + ivec2( -1, 0 ) ) + w0 * at( p ) + w1 * at( p + ivec2( 1, 0 ) );
	float row2 = w1 * at( p + ivec2( -1, 1 ) ) + w0 * at( p + ivec2( 0, 1 ) ) + w1 * at( p + ivec2( 1, 1 ) );
	float d    = w1 * row0 + w0 * row1 + w1 * row2;
	fragColor  = vec4( 1.0 - d, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// composite: the last page as toner on paper, mixed with the input.
//---------------------------------------------------------------------------
const char* const kComposite = R"(#version 410 core

uniform sampler2D InputTexture;
uniform sampler2D PageTexture;
uniform int PageW;
uniform int PageH;
uniform vec3 Paper;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

void main()
{
	ivec2 p   = clamp( ivec2( floor( uv * vec2( PageW, PageH ) ) ), ivec2( 0 ), ivec2( PageW - 1, PageH - 1 ) );
	vec4 src  = texelFetch( InputTexture, p, 0 );
	float R   = texelFetch( PageTexture, p, 0 ).r;
	vec3 copy = Paper * R;
	// A page is opaque: a copier outputs a sheet, whatever the alpha of what was
	// on the platen. Mix fades back to the source, alpha included.
	fragColor = vec4( mix( src.rgb, copy, MixAmount ), mix( src.a, 1.0, MixAmount ) );
}
)";

//---------------------------------------------------------------------------
// The three process-direction passes are assembled from the shared library
// so the mapping is written once. (The field passes above declare their
// kernel arrays at 2 x model::kKernelRadius + 1; Toner.cpp asserts it.)
//---------------------------------------------------------------------------
namespace
{
std::string assemble( const char* main_ )
{
	return std::string( "#version 410 core\n" ) + kProcessLib + main_;
}
const std::string kCoverageText = assemble( kCoverageMain );
const std::string kSupplyText   = assemble( kSupplyMain );
const std::string kMarksText    = assemble( kMarksMain );
} // namespace

const char* const kCoverage = kCoverageText.c_str();
const char* const kSupply   = kSupplyText.c_str();
const char* const kMarks    = kMarksText.c_str();

} // namespace tonerfx::shaders
