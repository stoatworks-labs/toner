/**
 * Toner — browser demo.
 *
 * A photocopier, and a copy of a copy of a copy. The one idea, from
 * `source/Model.h`: each copy is the whole xerographic engine run once on the
 * last copy's page, and toner follows the FIELD above the latent image, not
 * the image — so greys collapse to paper or toner, solids go hollow because a
 * wide charged area's field is strongest at its edges, thin lines print full,
 * the developer runs short down a heavy page, drum marks repeat every
 * circumference, and each generation's seeded skew drifts the page.
 *
 * Like slope, this plugin is **mostly a shader**: every pixel is made in GLSL,
 * and the C++ converts sliders to physical units (`Controls.cpp`), computes the
 * coefficients in double once a frame (`Toner.cpp`, `Model.h`) and runs the
 * passes. So the two halves of this page are not equally faithful:
 *
 *   The shaders are the plugin's. The fourteen GLSL bodies below — the vertex
 *   shader, nine whole fragment shaders, the process-direction library and
 *   the three fragment mains assembled with it — are `kVertex`, `kIntake`,
 *   `kPlace`, `kBlur`, `kAE`, `kLatent`, `kFieldX`, `kDevelop`, `kProcessLib`,
 *   `kCoverageMain`, `kSupplyMain`, `kMarksMain`, `kFuse` and `kComposite`
 *   from `source/Shaders.cpp`, copied across unedited and assembled the way
 *   the plugin assembles them (`assembleProcess` below is Shaders.cpp's
 *   `assemble`). `demo/tools/check_shaders.py` compares all fourteen character
 *   for character and `tools/verify.sh` runs it.
 *
 *   The CPU half is a PORT — of `Controls.cpp` (every slider to its unit),
 *   the closed forms and the seeded tables in `Model.h` (the charge, the
 *   development window, the two Cauchy kernels, the gain, the hash, the skew
 *   per generation, the drum's defects), and the per-frame coefficient code
 *   and pass order in `Toner::ProcessOpenGL` — function for function, in the
 *   same double precision (a JavaScript number is an IEEE double, which is
 *   what the C++ computes in), rounded to float where the plugin hands a
 *   float uniform over. Nothing checks a port but a reader. `totest --laws`,
 *   `--fixedpoint`, `--fringe` and the rest check the C++ originals and have
 *   no idea this page exists.
 *
 * ------------------------------------------------------------- the buffers
 *
 * The plugin's page and work buffers are R32F, its field buffer RG32F, its
 * AE reading a 1 x 1 R32F, and its coverage and supply two L x 1 R32F
 * buffers along the process direction; every one Nearest, every read a
 * texelFetch. The page allocates exactly those through the kit's PassBuffer.
 * Rendering into a float texture is an extension in WebGL2
 * (EXT_color_buffer_float), so `needFloat` asks the kit for it, and the kit
 * refuses to start rather than fall back to 8 bits: a charge quantised to
 * 1/255 mid-chain would be a plausible wrong picture, which is the failure
 * these plugins' own harnesses exist to prevent.
 *
 * ------------------------------------------------------------- the clock
 *
 * There is none. The plugin declares `SetTimeSupported( false )`: every
 * generation's skew and offset and every drum defect are seeded by index, so
 * a frame is the same picture every time it is rendered. The kit's clock only
 * moves the generated clip; Pause, Step and Restart change nothing in the
 * copier itself.
 *
 * ------------------------------------------------------------- what is missing
 *
 * **Nothing audio.** Toner has no audio path. **The About block is absent**,
 * as on every page in this suite. **Generations is a dropdown**: it is
 * FF_TYPE_INTEGER 1..8 in the plugin and the kit has no integer control, so
 * — as copperlist, galvo and teletext did — it is a dropdown of the eight
 * values and the element index converts back. The harness-only `Perturb`
 * uniform is set to what the shipped plugin sets it to: 0.
 *
 * And what every page in this suite is not: this is the plugin's shaders and a
 * port of its C++, not the plugin. No Resolume, no composition, no FFGL, and
 * GLSL ES 3.00 in a browser rather than desktop GL 4.1 core.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here. Ten of them
// carry their own #version line; the three process-direction passes are
// assembled as version + library + main, exactly as Shaders.cpp's
// `assemble` does.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

const PROCESS_LIB = `
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
`;

const INTAKE = `#version 410 core

uniform sampler2D InputTexture;

out vec4 fragColor;

const vec3 kLuma = vec3( 0.299, 0.587, 0.114 );

void main()
{
	ivec2 p   = ivec2( gl_FragCoord.xy );
	vec4 c    = texelFetch( InputTexture, p, 0 );
	fragColor = vec4( dot( c.rgb, kLuma ), 0.0, 0.0, 1.0 );
}
`;

const PLACE = `#version 410 core

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
`;

const BLUR = `#version 410 core

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
`;

const AE = `#version 410 core

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
`;

const LATENT = `#version 410 core

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
`;

const FIELDX = `#version 410 core

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
`;

const DEVELOP = `#version 410 core

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
`;

const COVERAGE_MAIN = `
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
`;

const SUPPLY_MAIN = `
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
`;

const MARKS_MAIN = `
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
`;

const FUSE = `#version 410 core

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
`;

const COMPOSITE = `#version 410 core

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
`;

const assembleProcess = (main) => '#version 410 core\n' + PROCESS_LIB + main;
const COVERAGE = assembleProcess(COVERAGE_MAIN);
const SUPPLY = assembleProcess(SUPPLY_MAIN);
const MARKS = assembleProcess(MARKS_MAIN);

//===========================================================================
// Model.h, ported. The copier as numbers.
//===========================================================================

const K_GENERATIONS_MIN = 1;
const K_GENERATIONS_MAX = 8;

/// The discharge curve's exponent and the tone curve's unstable fixed point.
const K_PIDC = 2.0;
const K_MID_GREY = 0.5;

/// The latent charge for a reflectance R, 1 at black and 0 at paper.
function charge(R) {
  const floor_ = Math.exp(-K_PIDC);
  return (Math.exp(-K_PIDC * R) - floor_) / (1.0 - floor_);
}

/// dq/dR at mid-grey, negated.
function chargeSlopeAtMid() {
  return K_PIDC * Math.exp(-K_PIDC * K_MID_GREY) / (1.0 - Math.exp(-K_PIDC));
}

/// The development window for a tone-curve slope S at mid-grey.
function developmentWindow(slope) {
  const mid = charge(K_MID_GREY);
  const width = chargeSlopeAtMid() / slope;
  return { lo: mid - width / 2.0, hi: mid + width / 2.0 };
}

/// The field: the development gap and the photoconductor's thickness in
/// pixels, the kernel radius, and the truncated, renormalised Cauchy kernel.
const K_GAP_PX = 0.5;
const K_THICKNESS_PX = 0.5;
const K_PI = 3.14159265358979323846;
const K_KERNEL_RADIUS = 16;

const cauchyTexel = (j, h) => (Math.atan((j + 0.5) / h) - Math.atan((j - 0.5) / h)) / K_PI;
const cauchyMass = (h) => 2.0 * Math.atan((K_KERNEL_RADIUS + 0.5) / h) / K_PI;
const kernelTexel = (j, h) => cauchyTexel(j, h) / cauchyMass(h);

/// One over the field of a one-pixel line at full charge.
function fieldGain() {
  const H = K_GAP_PX + 2.0 * K_THICKNESS_PX;
  return 1.0 / (kernelTexel(0, K_GAP_PX) - kernelTexel(0, H));
}

/// The rest of the machine.
const K_FUSE_SPREAD = 0.15;
const K_AE_FLOOR = 1.0 / 32.0;
const K_DRUM_DEFECTS = 8;
const K_DRUM_SPECKS = 5;
const K_DRUM_SIGMA_MIN = 1.0;
const K_DRUM_SIGMA_MAX = 3.0;

/// Seeds. Arbitrary and fixed: 'TON', 1 and 2.
const K_SKEW_SEED = 0x544f4e01;
const K_DRUM_SEED = 0x544f4e02;

/// Options.
const PAPER_NAMES = ['White', 'Cream', 'Goldenrod', 'Pink', 'Blue', 'Green', 'Newsprint'];
const PAPER_RGB = [
  [1.0, 1.0, 1.0], [1.0, 0.97, 0.88], [1.0, 0.82, 0.35], [1.0, 0.80, 0.85],
  [0.75, 0.85, 1.0], [0.78, 0.95, 0.80], [0.85, 0.83, 0.78],
];
const DIRECTION_NAMES = ['Down', 'Up', 'Right', 'Left'];
const K_RIGHT = 2;

/// The fleet's PCG output mix, in 32-bit unsigned arithmetic: Math.imul
/// wraps as the C++ uint32_t does, and >>> 0 keeps every intermediate
/// unsigned.
function hashInt(v) {
  const state = (Math.imul(v >>> 0, 747796405) + 2891336453) >>> 0;
  const word = Math.imul((state >>> ((state >>> 28) + 4)) ^ state, 277803737) >>> 0;
  return ((word >>> 22) ^ word) >>> 0;
}

/// The top 24 bits as 0..1, exact in a float; and -1..1.
const unitOf = (h) => (h >>> 8) * (1.0 / 16777216.0);
const signedOf = (h) => unitOf(h) * 2.0 - 1.0;

/// Generation g's (1-based) skew as a fraction of the maximum, and its
/// offset as fractions of the offset amplitude, each in -1..1.
const skewFraction = (g) => signedOf(hashInt((K_SKEW_SEED + 3 * g) >>> 0));
const offsetXFraction = (g) => signedOf(hashInt((K_SKEW_SEED + 3 * g + 1) >>> 0));
const offsetYFraction = (g) => signedOf(hashInt((K_SKEW_SEED + 3 * g + 2) >>> 0));

/// Defect i's position round the circumference and across the page, both as
/// fractions, its sigma in pixels, and whether it is a speck.
function drumDefect(i) {
  const base = (K_DRUM_SEED + 4 * i) >>> 0;
  return {
    u: unitOf(hashInt(base)),
    v: unitOf(hashInt((base + 1) >>> 0)),
    sigma: K_DRUM_SIGMA_MIN + (K_DRUM_SIGMA_MAX - K_DRUM_SIGMA_MIN) * unitOf(hashInt((base + 2) >>> 0)),
    speck: i < K_DRUM_SPECKS,
  };
}

//===========================================================================
// Controls.cpp, ported. What a host parameter means.
//
// The plugin stores every host value as a `float`, and each law takes that
// float. The page's values are doubles from a slider, so each is rounded
// through Math.fround first — the same 24-bit value the plugin holds — before
// the law is applied in double, as the C++ does.
//===========================================================================

const unit = (value) => Math.min(Math.max(Math.fround(value), 0.0), 1.0);
const lround = (value) => (value < 0 ? -Math.round(-value) : Math.round(value));

const optionIndex = (value, count) => Math.min(Math.max(lround(Math.fround(value)), 0), count - 1);
const generationsOf = (value) => Math.min(Math.max(lround(Math.fround(value)), K_GENERATIONS_MIN), K_GENERATIONS_MAX);
const contrastSlope = (value) => 1.6 * Math.pow(10.0, unit(value));
const bgSuppression = (value) => unit(value);
const opticsSigma = (value) => { const v = unit(value); return 4.0 * v * v; };
const zoomOf = (value) => 1.0 + 0.08 * (unit(value) - 0.5);
const skewDegrees = (value) => 2.0 * unit(value);
const offsetFraction = (value) => 0.005 * unit(value);
const solidFill = (value) => unit(value);
const depletionPerPage = (value) => { const v = 1.0 - unit(value); return 6.0 * v * v; };
const recoveryPerPage = (value) => { const v = unit(value); return 6.0 * v * v; };
const screenPitchPx = (value) => 4.0 * Math.pow(2.0, 2.0 * unit(value));
const screenAngleDegrees = (value) => 180.0 * unit(value);
const drumMarks = (value) => unit(value);
const circumferenceFraction = (value) => 0.1 + 0.9 * unit(value);
const perRow = (perPage, rows) => (rows > 0 ? perPage / rows : 0.0);

//===========================================================================
// The renderer: Toner::ProcessOpenGL, in its order. Once a frame the
// coefficients in double; then intake, and per generation the eleven passes
//
//   place, blur x, blur y, ae, latent, fieldx, develop, coverage, supply,
//   marks, fuse
//
// between the two page buffers; then the composite onto the canvas.
//===========================================================================

/// What the line under the canvas reports. Filled by the renderer.
const telemetry = { generations: 0, width: 0, height: 0, length: 0, circumference: 0, windowLo: 0, windowHi: 0, gain: 0, blurRadius: 0 };

/// The harness-only negative-control bitmask, always 0 in the shipped plugin.
const PERTURB = 0;

function createRenderer(gl, quad) {
  const program = (fragment, label) => new Program(gl, VERTEX, fragment, label);
  const intakeShader = program(INTAKE, 'intake');
  const placeShader = program(PLACE, 'place');
  const blurShader = program(BLUR, 'blur');
  const aeShader = program(AE, 'ae');
  const latentShader = program(LATENT, 'latent');
  const fieldXShader = program(FIELDX, 'fieldx');
  const developShader = program(DEVELOP, 'develop');
  const coverageShader = program(COVERAGE, 'coverage');
  const supplyShader = program(SUPPLY, 'supply');
  const marksShader = program(MARKS, 'marks');
  const fuseShader = program(FUSE, 'fuse');
  const compositeShader = program(COMPOSITE, 'composite');

  // The buffers, all Nearest, as the plugin's PassBuffer::Ensure( …, Nearest ).
  const nearest = { filter: 'nearest' };
  const pages = [new PassBuffer(gl, nearest), new PassBuffer(gl, nearest)];
  const work = [new PassBuffer(gl, nearest), new PassBuffer(gl, nearest)];
  const field = new PassBuffer(gl, nearest);
  const ae = new PassBuffer(gl, nearest);
  const coverage = new PassBuffer(gl, nearest);
  const supply = new PassBuffer(gl, nearest);

  // The float arrays the plugin uploads with glUniform1fv / glUniform1iv.
  const blurWeights = new Float32Array(13);
  const kernelH = new Float32Array(2 * K_KERNEL_RADIUS + 1);
  const kernelHH = new Float32Array(2 * K_KERNEL_RADIUS + 1);
  const defectU = new Float32Array(K_DRUM_DEFECTS);
  const defectV = new Float32Array(K_DRUM_DEFECTS);
  const defectSigma = new Float32Array(K_DRUM_DEFECTS);
  const defectSpeck = new Int32Array(K_DRUM_DEFECTS);

  /// `glUniform1iv` for the one integer array: the kit's setArray is floats only.
  const setIntArray = (shader, name, values) => {
    const loc = shader.location(`${name}[0]`) ?? shader.location(name);
    if (loc !== null) gl.uniform1iv(loc, values);
  };

  /// A pass: the target bound and sized, the shader in use, each texture on
  /// its unit from 0 up — Toner.cpp's bindTarget + bindTextures.
  const pass = (target, shader, textures) => {
    target.bind();
    shader.use();
    textures.forEach((texture, unit) => bindTexture(gl, unit, texture));
    gl.activeTexture(gl.TEXTURE0);
  };

  return {
    render({ input, params, width: vpW, height: vpH }) {
      const p = (id) => params.get(id);
      const picture = input;
      const W = picture.width;
      const H = picture.height;

      //------------------------------------------------------------------
      // The settings, in physical units.
      //------------------------------------------------------------------
      const generations = generationsOf(integerValue('generations', p('generations')));
      const slope = contrastSlope(p('contrast'));
      const suppress = bgSuppression(p('bgSuppression'));
      const sigma = opticsSigma(p('optics'));
      const zoom = zoomOf(p('zoom'));
      const skewMax = skewDegrees(p('skew'));
      const offsetAmp = offsetFraction(p('skew')) * Math.min(W, H);
      const electrode = solidFill(p('solidFill'));
      const depletion = depletionPerPage(p('tonerSupply'));
      const recovery = recoveryPerPage(p('recovery'));
      const photoMode = Math.fround(p('photoMode')) >= 0.5;
      const pitch = screenPitchPx(p('screen'));
      const screenDeg = screenAngleDegrees(p('screenAngle'));
      const paper = optionIndex(p('paper'), PAPER_NAMES.length);
      const marks = drumMarks(p('drumMarks'));
      const circumFrac = circumferenceFraction(p('circumference'));
      const direction = optionIndex(p('direction'), DIRECTION_NAMES.length);

      const L = direction < K_RIGHT ? H : W; // along the process direction
      const A = direction < K_RIGHT ? W : H; // across it

      //------------------------------------------------------------------
      // Buffers. Every allocation before anything binds a texture.
      //------------------------------------------------------------------
      pages[0].ensure(W, H, gl.R32F);
      pages[1].ensure(W, H, gl.R32F);
      work[0].ensure(W, H, gl.R32F);
      work[1].ensure(W, H, gl.R32F);
      field.ensure(W, H, gl.RG32F);
      ae.ensure(1, 1, gl.R32F);
      coverage.ensure(L, 1, gl.R32F);
      supply.ensure(L, 1, gl.R32F);

      //------------------------------------------------------------------
      // The coefficients, in double, once a frame.
      //------------------------------------------------------------------
      // The optics: a Gaussian to three sigma, at most 12 either side.
      const blurRadius = sigma > 0.0 ? Math.min(12, Math.ceil(3.0 * sigma)) : 0;
      {
        const weights = new Float64Array(13);
        weights[0] = 1.0;
        let total = 1.0;
        for (let j = 1; j <= blurRadius; j += 1) {
          weights[j] = Math.exp(-(j * j) / (2.0 * sigma * sigma));
          total += 2.0 * weights[j];
        }
        blurWeights.fill(0);
        blurWeights[0] = 1.0;
        for (let j = 0; j <= blurRadius; j += 1) blurWeights[j] = weights[j] / total;
      }

      // The two field kernels and the gain.
      {
        const farGap = K_GAP_PX + 2.0 * K_THICKNESS_PX;
        for (let j = -K_KERNEL_RADIUS; j <= K_KERNEL_RADIUS; j += 1) {
          kernelH[j + K_KERNEL_RADIUS] = kernelTexel(j, K_GAP_PX);
          kernelHH[j + K_KERNEL_RADIUS] = kernelTexel(j, farGap);
        }
      }
      const gain = fieldGain();

      // The development window.
      const { lo: windowLo, hi: windowHi } = developmentWindow(slope);

      // The drum: defects in process pixels, the circumference in pixels.
      const circumference = circumFrac * L;
      for (let i = 0; i < K_DRUM_DEFECTS; i += 1) {
        const d = drumDefect(i);
        defectU[i] = d.u * circumference;
        defectV[i] = d.v * A;
        defectSigma[i] = d.sigma;
        defectSpeck[i] = d.speck ? 1 : 0;
      }

      const screenRad = screenDeg * K_PI / 180.0;

      gl.disable(gl.BLEND);

      //------------------------------------------------------------------
      // Intake: the picture as page 0.
      //------------------------------------------------------------------
      pass(pages[0], intakeShader, [picture.texture]);
      intakeShader.setSampler('InputTexture', 0);
      quad.draw();

      //------------------------------------------------------------------
      // The generations.
      //------------------------------------------------------------------
      for (let g = 1; g <= generations; g += 1) {
        const pageIn = pages[(g - 1) & 1];
        const pageOut = pages[g & 1];

        // This generation's placement: the inverse map is the rotation by
        // -theta over the zoom, as a float matrix.
        const seedGen = (PERTURB & 2) ? 1 : g;
        const theta = skewMax * skewFraction(seedGen) * K_PI / 180.0;
        const dx = offsetAmp * offsetXFraction(seedGen);
        const dy = offsetAmp * offsetYFraction(seedGen);
        const c = Math.cos(theta) / zoom;
        const s = Math.sin(theta) / zoom;

        // 1. place
        pass(work[0], placeShader, [pageIn.texture]);
        placeShader.setSampler('PageTexture', 0);
        placeShader.setInt('PageW', W);
        placeShader.setInt('PageH', H);
        placeShader.set('Inv', Math.fround(c), Math.fround(s), Math.fround(-s), Math.fround(c));
        placeShader.set('Centre', Math.fround(W * 0.5), Math.fround(H * 0.5));
        placeShader.set('Offset', Math.fround(dx), Math.fround(dy));
        quad.draw();

        // 2. optics, x then y: work0 -> work1 -> work0
        for (let axis = 0; axis < 2; axis += 1) {
          pass(work[1 - axis], blurShader, [work[axis].texture]);
          blurShader.setSampler('PageTexture', 0);
          blurShader.setInt('PageW', W);
          blurShader.setInt('PageH', H);
          blurShader.setInt('Axis', axis);
          blurShader.setInt('Radius', blurRadius);
          blurShader.setArray('Weights', blurWeights, 1);
          quad.draw();
        }

        // 3. the AE sensor
        pass(ae, aeShader, [work[0].texture]);
        aeShader.setSampler('PageTexture', 0);
        aeShader.setInt('PageW', W);
        aeShader.setInt('PageH', H);
        aeShader.set('AEFloor', Math.fround(K_AE_FLOOR));
        aeShader.setInt('Perturb', PERTURB);
        quad.draw();

        // 4. the latent image: work0 + ae -> work1
        pass(work[1], latentShader, [work[0].texture, ae.texture]);
        latentShader.setSampler('PageTexture', 0);
        latentShader.setSampler('AETexture', 1);
        latentShader.set('Suppression', Math.fround(suppress));
        latentShader.setInt('PhotoMode', photoMode ? 1 : 0);
        latentShader.set('ScreenPitch', Math.fround(pitch));
        latentShader.set('ScreenAxis', Math.fround(Math.cos(screenRad)), Math.fround(Math.sin(screenRad)));
        latentShader.set('Pidc', Math.fround(K_PIDC));
        latentShader.set('PidcFloor', Math.fround(Math.exp(-K_PIDC)));
        latentShader.setInt('Perturb', PERTURB);
        quad.draw();

        // 5. the field's x pass: work1 (q) -> field
        pass(field, fieldXShader, [work[1].texture]);
        fieldXShader.setSampler('ChargeTexture', 0);
        fieldXShader.setInt('PageW', W);
        fieldXShader.setInt('PageH', H);
        fieldXShader.setInt('Radius', K_KERNEL_RADIUS);
        fieldXShader.setArray('KernelH', kernelH, 1);
        fieldXShader.setArray('KernelHH', kernelHH, 1);
        quad.draw();

        // 6. development: field + work1 (q) -> work0 (D)
        pass(work[0], developShader, [field.texture, work[1].texture]);
        developShader.setSampler('FieldTexture', 0);
        developShader.setSampler('ChargeTexture', 1);
        developShader.setInt('PageW', W);
        developShader.setInt('PageH', H);
        developShader.setInt('Radius', K_KERNEL_RADIUS);
        developShader.setArray('KernelH', kernelH, 1);
        developShader.setArray('KernelHH', kernelHH, 1);
        developShader.set('Gain', Math.fround(gain));
        developShader.set('Electrode', Math.fround(electrode));
        developShader.set('WindowLo', Math.fround(windowLo));
        developShader.set('WindowHi', Math.fround(windowHi));
        developShader.setInt('Perturb', PERTURB);
        quad.draw();

        // 7. coverage: work0 (D) -> coverage (L x 1)
        pass(coverage, coverageShader, [work[0].texture]);
        coverageShader.setSampler('DensityTexture', 0);
        coverageShader.setInt('Direction', direction);
        coverageShader.setInt('PageW', W);
        coverageShader.setInt('PageH', H);
        quad.draw();

        // 8. supply: coverage -> supply (L x 1)
        pass(supply, supplyShader, [coverage.texture]);
        supplyShader.setSampler('CoverageTexture', 0);
        supplyShader.setInt('Direction', direction);
        supplyShader.setInt('PageW', W);
        supplyShader.setInt('PageH', H);
        supplyShader.set('Recovery', Math.fround(perRow(recovery, L)));
        supplyShader.set('Depletion', Math.fround(perRow(depletion, L)));
        supplyShader.setInt('Perturb', PERTURB);
        quad.draw();

        // 9. starvation and the drum: work0 (D) + supply -> work1 (D')
        pass(work[1], marksShader, [work[0].texture, supply.texture]);
        marksShader.setSampler('DensityTexture', 0);
        marksShader.setSampler('SupplyTexture', 1);
        marksShader.setInt('Direction', direction);
        marksShader.setInt('PageW', W);
        marksShader.setInt('PageH', H);
        marksShader.set('Circumference', Math.fround(circumference));
        marksShader.set('Amplitude', Math.fround(marks));
        marksShader.setArray('DefectU', defectU, 1);
        marksShader.setArray('DefectV', defectV, 1);
        marksShader.setArray('DefectSigma', defectSigma, 1);
        setIntArray(marksShader, 'DefectSpeck', defectSpeck);
        marksShader.setInt('Perturb', PERTURB);
        quad.draw();

        // 10. fuse: work1 (D') -> page out
        pass(pageOut, fuseShader, [work[1].texture]);
        fuseShader.setSampler('DensityTexture', 0);
        fuseShader.setInt('PageW', W);
        fuseShader.setInt('PageH', H);
        fuseShader.set('Spread', Math.fround(K_FUSE_SPREAD));
        quad.draw();
      }

      //------------------------------------------------------------------
      // Composite, onto the canvas. The host's viewport is the whole canvas.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, vpW, vpH);
      compositeShader.use();
      bindTexture(gl, 0, picture.texture);
      bindTexture(gl, 1, pages[generations & 1].texture);
      compositeShader.setSampler('InputTexture', 0);
      compositeShader.setSampler('PageTexture', 1);
      compositeShader.setInt('PageW', W);
      compositeShader.setInt('PageH', H);
      compositeShader.set('Paper', PAPER_RGB[paper][0], PAPER_RGB[paper][1], PAPER_RGB[paper][2]);
      compositeShader.set('MixAmount', Math.fround(p('mix')));
      quad.draw();

      // Unbind so nothing reads a framebuffer's own texture next frame.
      bindTexture(gl, 1, null);
      bindTexture(gl, 0, null);

      telemetry.generations = generations;
      telemetry.width = W;
      telemetry.height = H;
      telemetry.length = L;
      telemetry.circumference = circumference;
      telemetry.windowLo = windowLo;
      telemetry.windowHi = windowHi;
      telemetry.gain = gain;
      telemetry.blurRadius = blurRadius;
    },
  };
}

//===========================================================================
// The controls, read out of Toner::Toner(). Same names, same groups, same
// order, same defaults, same dropdown elements. Absent: the About block.
//===========================================================================

/// FF_TYPE_INTEGER is exempt from the 0..1 clamp, so the plugin stores
/// Generations as the integer itself, 1..8. The kit has no integer control,
/// so -- as copperlist, galvo and teletext did -- it is a dropdown of every
/// value in the plugin's range; `integerValue` turns the dropdown's index
/// back into the integer.
const INTEGER_RANGES = {
  generations: [K_GENERATIONS_MIN, K_GENERATIONS_MAX],
};
const INTEGER_ELEMENTS = {};
for (const [id, [low, high]] of Object.entries(INTEGER_RANGES)) {
  INTEGER_ELEMENTS[id] = [];
  for (let v = low; v <= high; v += 1) INTEGER_ELEMENTS[id].push(String(v));
}
function integerValue(id, index) {
  const [low, high] = INTEGER_RANGES[id];
  return Math.min(high, Math.max(low, low + Math.round(index)));
}
const integerIndex = (id, value) => value - INTEGER_RANGES[id][0];

const integer = (id, name, value, group, hint) => ({ id, name, type: 'option', elements: INTEGER_ELEMENTS[id], default: integerIndex(id, value), group, hint });
const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });

const demo = mountDemo({
  name: 'Toner',
  pluginId: 'TO01',
  tagline:
    'A photocopier, and a copy of a copy of a copy. Each generation is the whole xerographic engine run on the last one’s page: optics that blur, an auto-exposure that throws the background away, a steep photoconductor, development that follows the electric field above the latent image — so solids go hollow and thin lines print full — a toner supply that runs short down the page, drum marks that repeat every circumference, and a seeded skew per copy. Up to eight generations a frame, and the look of a zine is the fixed point of that loop. The shaders here are the plugin’s own; the control laws and the coefficients are a port of its C++.',
  repo: 'https://github.com/stoatworks-labs/toner',
  page: 'https://stoatworks-labs.com/software/toner/',

  // The stock sentence says "same maths", which is only most of the truth
  // here: the shaders are the plugin's, the coefficients they run on are a port.
  blurb:
    'It is Toner’s own GLSL — all twelve fragment passes — ported from the repository to WebGL2, with the CPU half — every slider’s law, and the blur weights, the two field kernels, the development window, the placement matrix, the seeded skews and the drum’s defect table computed in double once a frame — ported to JavaScript by hand; nothing checks that port but a reader. It runs on generated clips in this page, with the plugin’s own parameters and no install.',

  // Every buffer in the chain is R32F or RG32F, as in the plugin: a latent
  // charge or a field quantised to 8 bits mid-chain would be a plausible
  // wrong picture.
  needFloat: true,

  params: [
    integer('generations', 'Generations', 3, 'Machine',
      'How many times the machine runs on its own output this frame, 1 to 8. FF_TYPE_INTEGER in the plugin; a dropdown of the same eight values here. Eight is where 1080p is still inside a 60 fps frame in the plugin.'),
    std('contrast', 'Contrast', 0.5, 'Machine', {
      display: (v) => `slope ${contrastSlope(v).toFixed(2)} at mid-grey`,
      hint: 'The tone curve’s slope at mid-grey, 1.6 × 10^v: 1.6 at 0, about 5.1 at the default, 16 at 1. It sets the development window’s width; every copy multiplies the transition by this.',
    }),
    std('bgSuppression', 'Bg Suppression', 0.6, 'Machine', {
      hint: 'How far the auto-exposure reading is trusted, 0 (the page is printed as it is) to 1 (the brightest sixteenth of the page is taken as paper white). The divisor is mix( 1, B, v ).',
    }),
    std('optics', 'Optics', 0.5, 'Machine', {
      display: (v) => `σ ${opticsSigma(v).toFixed(2)} px`,
      hint: 'The lens: a Gaussian blur of sigma 4v² pixels, none at 0, one pixel at the default, four at 1. Applied every generation.',
    }),
    std('zoom', 'Zoom', 0.5, 'Machine', {
      display: (v) => `${(100 * zoomOf(v)).toFixed(1)}% per copy`,
      hint: 'The scale per generation, 1 + 0.08( v − ½ ): 96% at 0, exactly 100% at the default, 104% at 1. It compounds across the generations.',
    }),
    std('skew', 'Skew', 0.3, 'Machine', {
      display: (v) => `up to ${skewDegrees(v).toFixed(2)}° and ${(100 * offsetFraction(v)).toFixed(2)}% of the short side`,
      hint: 'The largest rotation per generation, 2v degrees, and riding on it the largest offset, half a percent of the shorter side at 1. Each generation draws its own seeded fraction of both, so the drift is the same every frame. Positive is anticlockwise as seen.',
    }),

    std('solidFill', 'Solid Fill', 0.5, 'Development', {
      hint: 'The developer electrode: how far the field is flattened toward the parallel-plate value. The interior of a wide solid develops at Solid Fill × charge, so below the window’s top (0.375 at the default Contrast) every solid hollows into a rim over the generations, and above it pure black holds.',
    }),
    std('tonerSupply', 'Toner Supply', 0.85, 'Development', {
      display: (v) => `depletion ${depletionPerPage(v).toFixed(2)} per page`,
      hint: 'The developer’s depletion per unit coverage over a whole page, 6( 1 − v )²: six at 0, none at 1. Below about 0.7 a heavy page runs out of toner part-way down, and through the copy loop the fade becomes a cut.',
    }),
    std('recovery', 'Recovery', 0.5, 'Development', {
      display: (v) => `${recoveryPerPage(v).toFixed(2)} per page`,
      hint: 'How fast the developer recovers along the page, 6v² over a whole page. With depletion it sets the concentration a page of constant coverage settles to.',
    }),

    bool('photoMode', 'Photo Mode', 0, 'Photo',
      'Screen the page before the photoconductor: diamond dots whose area is exactly the tone. Re-screened every generation, the screen beats against itself.'),
    std('screen', 'Screen', 0.4, 'Photo', {
      display: (v) => `${screenPitchPx(v).toFixed(1)} px cells`,
      hint: 'The halftone cell’s pitch in pixels, 4 × 2^( 2v ): 4 to 16, about 7 at the default. Photo Mode only.',
    }),
    std('screenAngle', 'Screen Angle', 0.25, 'Photo', {
      display: (v) => `${screenAngleDegrees(v).toFixed(0)}°`,
      hint: '180v degrees, 45° at the default. A square lattice repeats every quarter turn, so 0 and 180 are the same screen. Photo Mode only.',
    }),

    opt('paper', 'Paper', PAPER_NAMES, 0, 'Page',
      'The stock the toner is fused to: the page out is paper × reflectance.'),
    std('drumMarks', 'Drum Marks', 0.3, 'Page', {
      hint: 'The amplitude of the drum’s eight seeded defects — five toner specks added, three dropouts multiplied — each a Gaussian of 1 to 3 pixels, repeating every circumference down the process direction.',
    }),
    std('circumference', 'Circumference', 0.25, 'Page', {
      display: (v) => `${(100 * circumferenceFraction(v)).toFixed(1)}% of the page`,
      hint: 'The drum’s circumference as a fraction of the page along the process direction, 0.1 + 0.9v, so the marks repeat this often.',
    }),
    opt('direction', 'Direction', DIRECTION_NAMES, 0, 'Page',
      'Which way the paper moves under the drum: the direction starvation deepens in and the drum’s repeats run along.'),
    std('mix', 'Mix', 1.0, 'Page', {
      hint: 'The copy against the input, alpha included: a page is opaque, whatever was on the platen.',
    }),
  ],

  // Solid shapes and text-like detail are what a copier is for; the scene
  // moves, so the seeded drift reads as a fixed transform rather than jitter.
  sources: ['scene', 'grid', 'bars', 'ramp', 'detail', 'spot'],

  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the controls.
  presets: {
    'One copy': { generations: integerIndex('generations', 1) },
    'Eighth generation': { generations: integerIndex('generations', 8) },
    'Hollow solids': { solidFill: 0.3 },
    'Running out of toner': { tonerSupply: 0.55 },
    'Photo Mode': { photoMode: 1 },
    'Goldenrod flyer': { paper: 2, drumMarks: 0.6, solidFill: 0.3 },
    'Fed sideways': { direction: 2 },
    'The hero frame': { generations: integerIndex('generations', 4), solidFill: 0.3, tonerSupply: 0.55, drumMarks: 0.6 },
  },

  differences: [
    'The CPU half of this plugin is a PORT, not the plugin’s own code. Toner computes every coefficient in double once a frame — the blur weights, the two 33-tap Cauchy field kernels and their gain, the development window from Contrast, the placement matrix from each generation’s seeded skew and offset, the drum’s seeded defect table, the per-row starvation rates — and converts every slider by its own law in Controls.cpp. All of that is ported here function for function, in JavaScript doubles, rounded to float where the plugin hands a float uniform over. Nothing checks a port but a reader; the repository’s totest --laws checks the C++ against the model’s statement and has never heard of this page.',
    'The GPU half is not a port. All twelve fragment passes and the vertex shader are the plugin’s own GLSL, assembled as the plugin assembles them, and demo/tools/check_shaders.py fails the repository’s verify script if a character of any of the fourteen bodies drifts.',
    'The buffers are the plugin’s: R32F pages and work buffers, an RG32F field, a 1 × 1 AE reading and two L × 1 buffers along the process direction, every one Nearest and read by texelFetch. WebGL2 renders into float textures only with EXT_color_buffer_float; the page refuses to start without it rather than fall back to 8 bits.',
    'Generations is FF_TYPE_INTEGER in the plugin, 1 to 8, with a real range. The kit has no integer control, so it is a dropdown of the same eight values.',
    'The plugin stores each host value as a float; the page’s sliders are doubles, so every value is rounded through Math.fround before its law is applied, and the defaults are the plugin’s float defaults.',
    'There is no clock in the plugin (SetTimeSupported( false )) and none in this copier: skews and drum marks are seeded by index, so Pause, Step and Restart move only the generated clip.',
    'The harness-only Perturb uniform is set to what the shipped plugin sets it to, 0. The six negative controls totest drives through it are not on this page.',
    'There is no audio caveat on this page: Toner has no audio path. The About block is absent, as on every page in this suite.',
    'The plugin’s proof — a ramp copied n times against the tone curve composed n times, a solid’s field against the strip closed form, the drum’s repeats one circumference apart, starvation against the depletion–recovery closed form, the skew against the sum of the seeded skews, the auto-exposure — is an offline harness in the repository, at two rasters. Nothing on this page measures anything; the line under the canvas reports what the port computed.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The line under the canvas. It reports the ported coefficient half's own
// numbers: how many generations ran and on what page, the development window
// the Contrast law produced, the field gain, and the drum's circumference in
// pixels. Skipped in embed mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);
    setInterval(() => {
      const { generations, width, height, length, circumference, windowLo, windowHi, gain, blurRadius } = telemetry;
      if (!generations) return;
      line.textContent =
        `${generations} generation${generations === 1 ? '' : 's'} × 11 passes on a ${width} × ${height} page, ${length} rows along the process direction. `
        + `Window ${windowLo.toFixed(3)}..${windowHi.toFixed(3)} of charge, field gain ${gain.toFixed(2)}, `
        + `optics ${blurRadius === 0 ? 'off' : `${2 * blurRadius + 1} taps`}, drum circumference ${circumference.toFixed(1)} px.`;
    }, 250);
  }
}
