/**
	totest -- render Toner offline, and read the copier back out of it.

	Every check here drives the REAL plugin class through a headless GL
	context and measures the answer out of the picture it made:

		totest --out /tmp/frame.png     a picture, on the moving test card
		totest --list                   every parameter, its kind and default
		totest --fixedpoint             a grey ramp copied n times is the stated
		                                tone curve composed n times: two plateaus,
		                                a transition that narrows every copy
		totest --fringe                 a solid square's centre-to-edge density
		                                ratio falls with its width as the strip
		                                field predicts; a one-pixel line prints
		                                full; the electrode removes the dip
		totest --drum                   the drum's repeats are spaced exactly one
		                                circumference apart, whole-pixel and
		                                fractional
		totest --starvation             density down a page of constant coverage
		                                follows the depletion-recovery closed form
		totest --skew                   after n generations a line has turned by
		                                the sum of the per-generation skews
		totest --ae                     grey paper comes out paper white, and a
		                                grey patch lands where T( patch / paper )
		                                says
		totest --negative               every check above can FAIL
		totest --laws --names           the checks that need no GL
		totest --bench                  the render cost
		totest --dump-shaders DIR       the exact GLSL the plugin compiles
		totest --pipe                   raw frames in, raw frames out

	The control laws, the tone curve, the field kernels, the starvation
	recurrence and the hash are stated HERE, from their definitions
	(Controls.h's comments and Model.h's description), and never read out of
	the plugin: a constant typed wrong there has to show up as a failed
	check, not as an agreement. AGENTS.md has one line per check on where
	each tolerance comes from.
*/

#include "Controls.h"
#include "Model.h"
#include "Shaders.h"
#include "Toner.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace model = tonerfx::model;

int g_checks   = 0;
int g_failures = 0;

constexpr double kU = 5.9604644775390625e-8;//2^-24, half a float ulp at 1

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The laws and the model, stated from their definitions. A check converts
// the FLOAT it hands the plugin, so the stated value is exactly what the
// plugin was asked for, and rounds the result through float where the
// plugin hands it to the GPU as a float uniform.
//---------------------------------------------------------------------------
double unit( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double asShader( double v )
{
	return static_cast< double >( static_cast< float >( v ) );
}
double statedSlope( float v )
{
	return 1.6 * std::pow( 10.0, unit( v ) );
}
double statedSigma( float v )
{
	return 4.0 * unit( v ) * unit( v );
}
double statedZoom( float v )
{
	return 1.0 + 0.08 * ( unit( v ) - 0.5 );
}
double statedSkewDeg( float v )
{
	return 2.0 * unit( v );
}
double statedOffsetFraction( float v )
{
	return 0.005 * unit( v );
}
double statedDepletion( float v )
{
	return 6.0 * ( 1.0 - unit( v ) ) * ( 1.0 - unit( v ) );
}
double statedRecovery( float v )
{
	return 6.0 * unit( v ) * unit( v );
}
double statedPitch( float v )
{
	return 4.0 * std::exp2( 2.0 * unit( v ) );
}
double statedCircumference( float v )
{
	return 0.1 + 0.9 * unit( v );
}
int statedGenerations( float v )
{
	return std::clamp( static_cast< int >( std::lround( v ) ), 1, 8 );
}

/// The photoconductor: q( R ), 1 at black, 0 at paper, k = 2.
constexpr double kPidc = 2.0;
double statedCharge( double R )
{
	const double floor_ = std::exp( -kPidc );
	return ( std::exp( -kPidc * R ) - floor_ ) / ( 1.0 - floor_ );
}
/// The development window for a slope S at mid-grey 0.5.
void statedWindow( double S, double& lo, double& hi )
{
	const double mid   = statedCharge( 0.5 );
	const double dq    = kPidc * std::exp( -kPidc * 0.5 ) / ( 1.0 - std::exp( -kPidc ) );
	const double width = dq / S;
	lo                 = mid - width / 2.0;
	hi                 = mid + width / 2.0;
}
/// One generation's tone curve with the electrode in: reflectance to
/// reflectance, through the uniforms as floats.
double statedTone( double R, float lo, float hi )
{
	const double q = std::clamp( statedCharge( R ), 0.0, 1.0 );
	const double D = std::clamp( ( q - lo ) / ( static_cast< double >( hi ) - lo ), 0.0, 1.0 );
	return 1.0 - D;
}

/// The field kernels: the Cauchy at scale h integrated over texel j,
/// truncated at radius 16 and normalised by the truncated mass.
constexpr int kR        = 16;
constexpr double kGap   = 0.5;
constexpr double kThick = 0.5;
double statedTexel( int j, double h )
{
	const double mass = 2.0 * std::atan( ( kR + 0.5 ) / h ) / M_PI;
	return ( std::atan( ( j + 0.5 ) / h ) - std::atan( ( j - 0.5 ) / h ) ) / M_PI / mass;
}
double statedGain()
{
	return 1.0 / ( statedTexel( 0, kGap ) - statedTexel( 0, kGap + 2.0 * kThick ) );
}
/// The 1-D response at offset x from the centre of a box of half-width a
/// (texels -a..a), for a kernel of scale h: the sum of the stated texels
/// that land in the box -- which is the strip closed form,
/// [ atan( ( a - x + 1/2 ) / h ) - atan( ( -a - x - 1/2 ) / h ) ] / pi / mass,
/// clipped to the window.
double boxResponse( int x, int a, double h )
{
	double sum = 0.0;
	for( int j = -kR; j <= kR; ++j )
		if( std::abs( x + j ) <= a )
			sum += asShader( statedTexel( j, h ) );
	return sum;
}

constexpr double kSpread = 0.15;

/// The fleet's hash, and the seeded tables.
uint32_t hashInt( uint32_t v )
{
	uint32_t state = v * 747796405u + 2891336453u;
	uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
double hashUnit( uint32_t h )
{
	return ( h >> 8 ) * ( 1.0 / 16777216.0 );
}
double hashSigned( uint32_t h )
{
	return hashUnit( h ) * 2.0 - 1.0;
}
constexpr uint32_t kSkewSeed = 0x544F4E01u;
constexpr uint32_t kDrumSeed = 0x544F4E02u;
double statedSkewFraction( int g )
{
	return hashSigned( hashInt( kSkewSeed + 3u * static_cast< uint32_t >( g ) ) );
}
struct StatedDefect
{
	double u, v, sigma;
	bool speck;
};
StatedDefect statedDefect( int i )
{
	const uint32_t base = kDrumSeed + 4u * static_cast< uint32_t >( i );
	StatedDefect d;
	d.u     = hashUnit( hashInt( base ) );
	d.v     = hashUnit( hashInt( base + 1u ) );
	d.sigma = 1.0 + 2.0 * hashUnit( hashInt( base + 2u ) );
	d.speck = i < 5;
	return d;
}

//---------------------------------------------------------------------------
// Pictures, float RGBA, top-first.
//---------------------------------------------------------------------------
using Picture = std::vector< float >;

Picture flat( int W, int H, double level )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( size_t i = 0; i < p.size(); i += 4 )
	{
		p[ i ] = p[ i + 1 ] = p[ i + 2 ] = static_cast< float >( level );
		p[ i + 3 ] = 1.0f;
	}
	return p;
}

void paint( Picture& p, int W, int H, int x0, int y0, int x1, int y1, double level )
{
	for( int y = std::max( 0, y0 ); y < std::min( H, y1 ); ++y )
		for( int x = std::max( 0, x0 ); x < std::min( W, x1 ); ++x )
		{
			float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
			px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( level );
		}
}

/// A horizontal ramp, ( x + 1/2 ) / W, black at the left.
Picture ramp( int W, int H )
{
	Picture p = flat( W, H, 0.0 );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
			px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( ( x + 0.5 ) / W );
		}
	return p;
}

float at( const std::vector< float >& img, int W, int r, int c, int ch = 0 )
{
	return img[ ( static_cast< size_t >( r ) * W + c ) * 4 + ch ];
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Toner::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Toner& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Toner::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range; an integer's range is real.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		else if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			p.low                   = range.min;
			p.high                  = range.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Toner& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Toner& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Toner& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

/// Every control a check can move, as the sliders the plugin sees. The
/// defaults here are the CLEAN machine: one generation, no blur, no AE, no
/// skew, exact zoom, the electrode fully in, an unlimited toner supply, no
/// drum marks, white paper. Each check moves the one thing it measures.
struct Knobs
{
	int generations    = 1;
	float contrast     = 0.5f;
	float suppression  = 0.0f;
	float optics       = 0.0f;
	float zoom         = 0.5f;
	float skew         = 0.0f;
	float solidFill    = 1.0f;
	float tonerSupply  = 1.0f;
	float recovery     = 0.0f;
	bool photoMode     = false;
	float screen       = 0.4f;
	float screenAngle  = 0.25f;
	int paper          = 0;
	float drumMarks    = 0.0f;
	float circumference = 0.25f;
	int direction      = 0;
	float mix          = 1.0f;
};

void apply( Toner& p, const Knobs& k )
{
	set( p, "Generations", static_cast< float >( k.generations ) );
	set( p, "Contrast", k.contrast );
	set( p, "Bg Suppression", k.suppression );
	set( p, "Optics", k.optics );
	set( p, "Zoom", k.zoom );
	set( p, "Skew", k.skew );
	set( p, "Solid Fill", k.solidFill );
	set( p, "Toner Supply", k.tonerSupply );
	set( p, "Recovery", k.recovery );
	set( p, "Photo Mode", k.photoMode ? 1.0f : 0.0f );
	set( p, "Screen", k.screen );
	set( p, "Screen Angle", k.screenAngle );
	set( p, "Paper", static_cast< float >( k.paper ) );
	set( p, "Drum Marks", k.drumMarks );
	set( p, "Circumference", k.circumference );
	set( p, "Direction", static_cast< float >( k.direction ) );
	set( p, "Mix", k.mix );
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output. No clock: the plugin has
// none.
//---------------------------------------------------------------------------
struct Session
{
	Toner plugin;
	int width        = 0;
	int height       = 0;
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip changes size: the SAME instance handed
	/// a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	bool renderNow()
	{
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed\n" );
		return ok;
	}

	bool render( const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderNow();
	}

	bool render( const Picture& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderNow();
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	std::vector< float > readBackFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

/// One frame of one picture through a fresh session, read back as floats.
bool renderOnce( const Knobs& k, int perturb, int W, int H, const Picture& pic, std::vector< float >& out )
{
	Session s;
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	if( !s.begin( W, H ) || !s.render( pic ) )
		return false;
	out = s.readBackFloat();
	s.end();
	return true;
}

/// The density the copier laid at ( r, c ): white paper, so 1 - R.
double densityAt( const std::vector< float >& out, int W, int r, int c )
{
	return 1.0 - static_cast< double >( at( out, W, r, c ) );
}

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

int report( bool ok, bool quiet, const char* format, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
int report( bool ok, bool quiet, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( quiet )
		return ok ? 0 : 1;
	va_list args;
	va_start( args, format );
	std::printf( "   %-4s ", verdict( ok ) );
	std::vprintf( format, args );
	std::printf( "\n" );
	va_end( args );
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// --fixedpoint: a grey ramp copied n times is the stated tone curve
// composed n times, spread and all. Measured per column, and as the plateau
// count and the transition's width.
//
// The prediction is a 1-D run of the per-column operations: the tone curve
// (the electrode is in, so the field is the charge), then the fuse's 3-tap
// along x (the page is uniform in y, so the y taps return the column). With
// the placement exact, no blur and no AE, nothing else touches a column.
//---------------------------------------------------------------------------
std::vector< double > predictRamp( int W, int n, float lo, float hi )
{
	std::vector< double > v( W );
	for( int x = 0; x < W; ++x )
		v[ x ] = asShader( ( x + 0.5 ) / W );
	const double w0 = asShader( 1.0 - 2.0 * asShader( kSpread ) );
	const double w1 = asShader( kSpread );
	for( int g = 0; g < n; ++g )
	{
		std::vector< double > d( W );
		for( int x = 0; x < W; ++x )
			d[ x ] = 1.0 - statedTone( v[ x ], lo, hi );
		for( int x = 0; x < W; ++x )
		{
			const double left  = d[ std::max( 0, x - 1 ) ];
			const double right = d[ std::min( W - 1, x + 1 ) ];
			v[ x ]             = 1.0 - ( w1 * left + w0 * d[ x ] + w1 * right );
		}
	}
	return v;
}

/// Plateaus: maximal runs of columns within `tol` of 0 or of 1. Returns the
/// count, and the width of everything between the first and the last.
int plateaus( const std::vector< double >& v, double tol, int& transition )
{
	int count     = 0;
	int lastLevel = -1;
	int firstEnd = -1, lastStart = -1;
	for( size_t x = 0; x < v.size(); ++x )
	{
		int level = -1;
		if( std::fabs( v[ x ] ) <= tol )
			level = 0;
		else if( std::fabs( 1.0 - v[ x ] ) <= tol )
			level = 1;
		if( level >= 0 && level != lastLevel )
			++count;
		if( level >= 0 && count == 1 )
			firstEnd = static_cast< int >( x );
		if( level >= 0 && count == 2 && lastStart < 0 )
			lastStart = static_cast< int >( x );
		lastLevel = level;
	}
	transition = ( firstEnd >= 0 && lastStart >= 0 ) ? lastStart - firstEnd - 1 : -1;
	return count;
}

int runFixedPoint( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== fixedpoint at %dx%d: a ramp copied n times is T composed n times\n", W, H );
	int failed = 0;
	const Picture pic = ramp( W, H );
	const int row     = H / 2;

	struct Case
	{
		float contrast;
		int generations;
	};
	const Case cases[] = { { 0.5f, 1 }, { 0.5f, 2 }, { 0.5f, 4 }, { 0.0f, 3 }, { 0.25f, 4 } };
	int lastTransition = -1;
	float lastContrast = -1.0f;
	for( const Case& c : cases )
	{
		Knobs k;
		k.contrast    = c.contrast;
		k.generations = c.generations;
		std::vector< float > out;
		if( !renderOnce( k, perturb, W, H, pic, out ) )
			return report( false, quiet, "render failed" );

		const double S = statedSlope( c.contrast );
		double lo = 0, hi = 1;
		statedWindow( S, lo, hi );
		const float flo = static_cast< float >( lo ), fhi = static_cast< float >( hi );
		const std::vector< double > pred = predictRamp( W, c.generations, flo, fhi );

		//Tolerance: one generation's charge carries the GPU's exp error, 3
		//ulp of a value below 1 (GLSL 4.10 8.2 allows 3 ulp for exp), over
		//( 1 - e^-2 ) and over the window's width; the slope S amplifies it
		//once per further generation: eps sum_{i<n} S^i.
		const double eps = 3.0 * 2.0 * kU / ( 1.0 - std::exp( -kPidc ) ) / ( fhi - flo ) + 4.0 * kU;
		double amp       = 0.0;
		for( int i = 0; i < c.generations; ++i )
			amp += std::pow( S, i );
		const double tol = eps * amp;

		double worst = 0.0;
		int worstX   = 0;
		std::vector< double > got( W );
		for( int x = 0; x < W; ++x )
		{
			got[ x ]       = at( out, W, row, x );
			const double e = std::fabs( got[ x ] - pred[ x ] );
			if( e > worst )
			{
				worst  = e;
				worstX = x;
			}
		}
		failed += report( worst <= tol, quiet, "contrast %.2f (S %.2f), %d generation%s: worst column error %.2e at x=%d (tolerance %.2e)",
		                  c.contrast, S, c.generations, c.generations == 1 ? "" : "s", worst, worstX, tol );

		int transition = -1, predTransition = -1;
		const int count     = plateaus( got, 1e-3, transition );
		const int predCount = plateaus( pred, 1e-3, predTransition );
		failed += report( count == 2 && predCount == 2, quiet, "  plateaus: %d measured, %d predicted (want 2: paper and toner)", count, predCount );
		//A column within the plateau tolerance of the threshold can land on
		//either side: one column at each end of the transition.
		failed += report( std::abs( transition - predTransition ) <= 2, quiet, "  transition: %d columns measured, %d predicted (+-2)", transition, predTransition );
		if( lastContrast == c.contrast && lastTransition >= 0 )
			failed += report( transition < lastTransition, quiet, "  narrower than the previous generation count's %d", lastTransition );
		lastTransition = transition;
		lastContrast   = c.contrast;
	}
	return failed;
}

//---------------------------------------------------------------------------
// --fringe: the field of a charged square, against the strip closed form.
//---------------------------------------------------------------------------
int runFringe( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== fringe at %dx%d: a solid's field against the strip closed form\n", W, H );
	int failed = 0;
	const double G = statedGain();
	double lo = 0, hi = 1;
	statedWindow( statedSlope( 0.5f ), lo, hi );
	const float flo = static_cast< float >( lo ), fhi = static_cast< float >( hi );
	const double H2 = kGap + 2.0 * kThick;

	//Tolerance: two 33-tap sums in float, each term below 1, twice (x and
	//y), through the gain and the window; plus the charge's exp error.
	const double tol = ( 2.0 * ( 2 * kR + 1 ) * 2.0 * kU * asShader( G ) + 3.0 * 2.0 * kU * asShader( G ) ) / ( fhi - flo ) + 4.0 * kU;

	const int cy = H / 2, cx = W / 2;
	const int halfWidths[] = { 1, 2, 4, 6, 8, 12, 16, 24 };
	std::vector< double > ratios, predRatios;
	for( int a : halfWidths )
	{
		if( 2 * a + 1 + 2 * kR + 4 > std::min( W, H ) )
			continue;
		Picture pic = flat( W, H, 1.0 );
		paint( pic, W, H, cx - a, cy - a, cx + a + 1, cy + a + 1, 0.0 );
		Knobs k;
		k.solidFill = 0.0f;
		std::vector< float > out;
		if( !renderOnce( k, perturb, W, H, pic, out ) )
			return report( false, quiet, "render failed" );

		//The predicted density along the centre row, and its worst error
		//over the row's middle (the square plus the kernel's reach either
		//side), after the fuse's spread -- a 3-tap in x and, on the centre
		//row, in y as well, so the prediction is the 2-D field through a
		//3 x 3 spread.
		auto D = [ & ]( int x, int y ) {
			const double e = G * ( boxResponse( x, a, kGap ) * boxResponse( y, a, kGap ) - boxResponse( x, a, H2 ) * boxResponse( y, a, H2 ) );
			return std::clamp( ( e - flo ) / ( static_cast< double >( fhi ) - flo ), 0.0, 1.0 );
		};
		const double w0 = asShader( 1.0 - 2.0 * asShader( kSpread ) ), w1 = asShader( kSpread );
		auto spread = [ & ]( int x, int y ) {
			double sum = 0.0;
			for( int j = -1; j <= 1; ++j )
				for( int i = -1; i <= 1; ++i )
					sum += ( i == 0 ? w0 : w1 ) * ( j == 0 ? w0 : w1 ) * D( x + i, y + j );
			return sum;
		};
		double worst = 0.0;
		int worstX   = 0;
		double edgePeak = 0.0, predEdgePeak = 0.0;
		const int reach = a + kR + 1;
		for( int x = -reach; x <= reach; ++x )
		{
			const double got  = densityAt( out, W, cy, cx + x );
			const double pred = spread( x, 0 );
			const double e    = std::fabs( got - pred );
			if( e > worst )
			{
				worst  = e;
				worstX = x;
			}
			if( std::abs( x ) <= a )
			{
				edgePeak     = std::max( edgePeak, got );
				predEdgePeak = std::max( predEdgePeak, pred );
			}
		}
		const double centre = densityAt( out, W, cy, cx ), predCentre = spread( 0, 0 );
		const double ratio = edgePeak > 0.0 ? centre / edgePeak : 1.0, predRatio = predEdgePeak > 0.0 ? predCentre / predEdgePeak : 1.0;
		ratios.push_back( ratio );
		predRatios.push_back( predRatio );
		failed += report( worst <= tol, quiet, "square of %2d: centre row worst error %.2e at x=%+d (tolerance %.2e); centre/edge %.4f predicted %.4f", 2 * a + 1, worst, worstX, tol, ratio, predRatio );

		//The halo: just outside the edge the field reverses, and nothing
		//develops there.
		const double outside = densityAt( out, W, cy, cx + a + 2 );
		failed += report( outside <= tol, quiet, "  two pixels outside the edge: density %.2e (paper)", outside );
	}

	//The ratio falls with width, measured, wherever the closed form says it
	//should by more than the tolerance.
	bool falls = true;
	for( size_t i = 1; i < ratios.size(); ++i )
		if( predRatios[ i ] < predRatios[ i - 1 ] - 4.0 * tol && !( ratios[ i ] < ratios[ i - 1 ] ) )
			falls = false;
	std::string list;
	for( double r : ratios )
	{
		char buffer[ 32 ];
		std::snprintf( buffer, sizeof( buffer ), " %.3f", r );
		list += buffer;
	}
	failed += report( falls && ratios.size() >= 4, quiet, "the centre/edge ratio falls with width:%s", list.c_str() );

	//A one-pixel line develops to full density: the same closed form with
	//a = 0 across and the whole kernel along, through the spread in x only
	//(the line is uniform in y).
	{
		Picture pic = flat( W, H, 1.0 );
		paint( pic, W, H, cx, 0, cx + 1, H, 0.0 );
		Knobs k;
		k.solidFill = 0.0f;
		std::vector< float > out;
		if( !renderOnce( k, perturb, W, H, pic, out ) )
			return report( false, quiet, "render failed" );
		double alongH = 0.0, alongHH = 0.0;
		for( int j = -kR; j <= kR; ++j )
		{
			alongH += asShader( statedTexel( j, kGap ) );
			alongHH += asShader( statedTexel( j, H2 ) );
		}
		auto D = [ & ]( int x ) {
			const double e = G * ( boxResponse( x, 0, kGap ) * alongH - boxResponse( x, 0, H2 ) * alongHH );
			return std::clamp( ( e - flo ) / ( static_cast< double >( fhi ) - flo ), 0.0, 1.0 );
		};
		const double w0 = asShader( 1.0 - 2.0 * asShader( kSpread ) ), w1 = asShader( kSpread );
		double worst = 0.0;
		for( int x = -kR - 1; x <= kR + 1; ++x )
			worst = std::max( worst, std::fabs( densityAt( out, W, cy, cx + x ) - ( w1 * D( x - 1 ) + w0 * D( x ) + w1 * D( x + 1 ) ) ) );
		failed += report( D( 0 ) == 1.0 && worst <= tol, quiet, "a one-pixel line: the closed form develops it to %.3f (solid) and its profile matches to %.2e", D( 0 ), worst );
	}

	//The electrode removes the dip.
	{
		const int a = 16;
		if( 2 * a + 1 + 2 * kR + 4 <= std::min( W, H ) )
		{
			Picture pic = flat( W, H, 1.0 );
			paint( pic, W, H, cx - a, cy - a, cx + a + 1, cy + a + 1, 0.0 );
			Knobs k;
			k.solidFill = 1.0f;
			std::vector< float > out;
			if( !renderOnce( k, perturb, W, H, pic, out ) )
				return report( false, quiet, "render failed" );
			const double centre = densityAt( out, W, cy, cx );
			failed += report( std::fabs( centre - 1.0 ) <= tol, quiet, "Solid Fill 1 on the square of %d: centre density %.6f (solid)", 2 * a + 1, centre );
		}
	}
	return failed;
}

//---------------------------------------------------------------------------
// --drum: the repeats are one circumference apart.
//---------------------------------------------------------------------------
struct Blob
{
	double u, v;///< centroid, in process coordinates (pixel centres at +0.5)
	double mass;
};

/// The density centroid in a window of half-size `half` about ( u0, v0 ),
/// process coordinates, Direction Down: u = row, v = column.
Blob centroid( const std::vector< float >& out, int W, int H, double u0, double v0, int half )
{
	Blob b   = { 0.0, 0.0, 0.0 };
	const int r0 = static_cast< int >( std::floor( u0 ) ) - half, c0 = static_cast< int >( std::floor( v0 ) ) - half;
	for( int r = r0; r <= r0 + 2 * half; ++r )
		for( int c = c0; c <= c0 + 2 * half; ++c )
		{
			if( r < 0 || c < 0 || r >= H || c >= W )
				continue;
			const double d = densityAt( out, W, r, c );
			b.mass += d;
			b.u += d * ( r + 0.5 );
			b.v += d * ( c + 0.5 );
		}
	if( b.mass > 0.0 )
	{
		b.u /= b.mass;
		b.v /= b.mass;
	}
	return b;
}

int runDrum( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== drum at %dx%d: repeats one circumference apart\n", W, H );
	int failed = 0;
	const Picture pic = flat( W, H, 1.0 );
	const int L = H, A = W;//Direction Down

	struct Case
	{
		float slider;
		const char* name;
	};
	const Case cases[] = { { 0.5f, "whole-pixel" }, { 0.3f, "fractional" } };
	for( const Case& c : cases )
	{
		const float C = static_cast< float >( statedCircumference( c.slider ) * L );//the uniform, as the plugin rounds it
		const bool whole = C == std::floor( C );
		if( ( c.slider == 0.5f ) != whole )
		{
			failed += report( false, quiet, "%s case: circumference %.6f px is %s -- pick another slider value", c.name, C, whole ? "whole" : "fractional" );
			continue;
		}
		Knobs k;
		k.drumMarks     = 1.0f;
		k.circumference = c.slider;
		std::vector< float > out;
		if( !renderOnce( k, perturb, W, H, pic, out ) )
			return report( false, quiet, "render failed" );

		//Every speck's repeats whose 4-sigma window is inside the page and
		//clear of every other defect's window.
		struct Mark
		{
			int defect;
			int m;
			double u, v, sigma;
		};
		std::vector< Mark > all;
		for( int i = 0; i < 8; ++i )
		{
			const StatedDefect d = statedDefect( i );
			for( int m = 0; d.u * C + m * C < L + 4.0 * d.sigma; ++m )
				all.push_back( { i, m, d.u * C + m * C, d.v * A, d.sigma } );
		}
		int measured = 0;
		double worstSpacing = 0.0, worstWindow = 0.0;
		for( int i = 0; i < 5; ++i )
		{
			std::vector< Mark > mine;
			for( const Mark& mk : all )
			{
				if( mk.defect != i )
					continue;
				const int half = static_cast< int >( std::ceil( 4.0 * mk.sigma ) ) + 1;
				if( mk.u - half < 0 || mk.u + half >= L || mk.v - half < 0 || mk.v + half >= A )
					continue;
				bool clear = true;
				for( const Mark& other : all )
					if( other.defect != i || other.m != mk.m )
						if( std::fabs( other.u - mk.u ) < half + 4.0 * other.sigma + 1 && std::fabs( other.v - mk.v ) < half + 4.0 * other.sigma + 1 )
							clear = false;
				if( clear )
					mine.push_back( mk );
			}
			for( size_t j = 1; j < mine.size(); ++j )
			{
				if( mine[ j ].m != mine[ j - 1 ].m + 1 )
					continue;
				const int half = static_cast< int >( std::ceil( 4.0 * mine[ j ].sigma ) ) + 1;
				const Blob a = centroid( out, W, H, mine[ j - 1 ].u, mine[ j - 1 ].v, half );
				const Blob b = centroid( out, W, H, mine[ j ].u, mine[ j ].v, half );
				if( a.mass <= 0.0 || b.mass <= 0.0 )
					continue;
				++measured;
				worstSpacing = std::max( worstSpacing, std::fabs( ( b.u - a.u ) - C ) );
				worstSpacing = std::max( worstSpacing, std::fabs( b.v - a.v ) );
				if( whole )
				{
					//The two windows, pixel for pixel.
					const int r0 = static_cast< int >( std::floor( mine[ j - 1 ].u ) ) - half, c0 = static_cast< int >( std::floor( mine[ j - 1 ].v ) ) - half;
					const int shift = static_cast< int >( C );
					for( int r = r0; r <= r0 + 2 * half; ++r )
						for( int cc = c0; cc <= c0 + 2 * half; ++cc )
							worstWindow = std::max( worstWindow, std::fabs( densityAt( out, W, r, cc ) - densityAt( out, W, r + shift, cc ) ) );
				}
			}
		}
		//Tolerance on the centroid: a Gaussian of sigma >= 1 sampled at
		//pixel centres has its centroid within exp( -2 pi^2 sigma^2 ) of
		//the true centre, and the fuse's spread is symmetric; the readback's
		//float error over an 81-pixel window over a mass >= 2 pi is below
		//1e-5 px. 1e-3 px is a hundred times either.
		const double tolC = 1e-3;
		failed += report( measured >= 1 && worstSpacing <= tolC, quiet, "%s case, C = %.3f px: %d consecutive repeats measured, worst spacing error %.2e px (tolerance %.0e)", c.name, C, measured, worstSpacing, tolC );
		if( whole )
		{
			//Two pixels a whole circumference apart see the same Gaussian
			//argument up to the rounding of ( u - u0 - mC ) at the page's
			//scale: an ulp of L in the offset, through the Gaussian's largest
			//slope 1/( sigma e^1/2 ), plus the exp's ulps.
			const double ulpL = std::ldexp( 1.0, static_cast< int >( std::floor( std::log2( L ) ) ) - 23 );
			const double tolW = 4.0 * ulpL * std::exp( -0.5 ) / 1.0 + 8.0 * kU;
			failed += report( worstWindow <= tolW, quiet, "  windows one circumference apart agree pixel for pixel: worst %.2e (tolerance %.2e)", worstWindow, tolW );
		}
	}
	return failed;
}

//---------------------------------------------------------------------------
// --starvation: density down a page of constant coverage.
//---------------------------------------------------------------------------
int runStarvation( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== starvation at %dx%d: the depletion-recovery closed form\n", W, H );
	int failed = 0;

	struct Case
	{
		float supply, recovery;
		int direction;
	};
	const Case cases[] = { { 0.3f, 0.4f, 0 }, { 0.0f, 0.0f, 0 }, { 0.5f, 0.6f, 2 }, { 0.3f, 0.4f, 1 } };
	for( const Case& c : cases )
	{
		const bool alongRows = c.direction < 2;//the process runs down (or up) the rows
		const int L = alongRows ? H : W;
		//Stripes across the process direction: every other line along it is
		//black, so every process row's coverage is exactly one half.
		Picture pic = flat( W, H, 1.0 );
		if( alongRows )
		{
			for( int x = 0; x < W; x += 2 )
				paint( pic, W, H, x, 0, x + 1, H, 0.0 );
		}
		else
		{
			for( int y = 0; y < H; y += 2 )
				paint( pic, W, H, 0, y, W, y + 1, 0.0 );
		}
		Knobs k;
		k.tonerSupply = c.supply;
		k.recovery    = c.recovery;
		k.direction   = c.direction;
		std::vector< float > out;
		if( !renderOnce( k, perturb, W, H, pic, out ) )
			return report( false, quiet, "render failed" );

		//The stated law: per-row rates as the plugin rounds them.
		const float r     = static_cast< float >( statedRecovery( c.recovery ) / L );
		const float kappa = static_cast< float >( statedDepletion( c.supply ) / L );
		const double a    = 0.5;
		const double lambda = static_cast< double >( r ) + static_cast< double >( kappa ) * a;
		const double cInf   = lambda > 0.0 ? r / lambda : 1.0;
		auto conc           = [ & ]( int j ) { return cInf + ( 1.0 - cInf ) * std::pow( 1.0 - lambda, j ); };

		//The stripes through the spread: a black line's column keeps 1 - 2s
		//and lends s to each white neighbour; along the process direction
		//the spread mixes three rows' concentrations.
		const double w0 = asShader( 1.0 - 2.0 * asShader( kSpread ) ), w1 = asShader( kSpread );
		auto stripe = [ & ]( int i, int n ) {
			auto black = [ & ]( int t ) { return std::clamp( t, 0, n - 1 ) % 2 == 0 ? 1.0 : 0.0; };
			return w1 * black( i - 1 ) + w0 * black( i ) + w1 * black( i + 1 );
		};
		auto rowConc = [ & ]( int j ) {
			return w1 * conc( std::max( 0, j - 1 ) ) + w0 * conc( j ) + w1 * conc( std::min( L - 1, j + 1 ) );
		};

		//Tolerance: the recurrence in float, j steps of three roundings on a
		//value at most 1 (plus the coverage sum's, exact here), plus the
		//spread's four roundings.
		double worst = 0.0;
		int worstJ   = 0;
		double tolAt = 0.0;
		const int across = alongRows ? W : H;
		for( int j = 0; j < L; ++j )
		{
			const double tol = 2.0 * kU * ( 3.0 * ( j + 1 ) + 4.0 ) + 4.0 * kU;
			for( int i = 0; i < across; i += 7 )
			{
				const double pred = stripe( i, across ) * rowConc( j );
				int row, col;
				if( c.direction == 0 )
				{
					row = j;
					col = i;
				}
				else if( c.direction == 1 )
				{
					row = H - 1 - j;
					col = i;
				}
				else
				{
					row = i;
					col = j;
				}
				const double e = std::fabs( densityAt( out, W, row, col ) - pred ) / tol;
				if( e > worst )
				{
					worst  = e;
					worstJ = j;
					tolAt  = tol;
				}
			}
		}
		failed += report( worst <= 1.0, quiet, "supply %.1f recovery %.1f direction %d: lambda %.3e/row, c_inf %.4f, c_L %.4f; worst %.3f of tolerance at row %d (%.1e)",
		                  c.supply, c.recovery, c.direction, lambda, cInf, conc( L - 1 ), worst, worstJ, tolAt );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --skew: a line turns by the sum of the per-generation skews.
//---------------------------------------------------------------------------
/// The line's angle from a least-squares fit of its per-column density
/// centroid over the middle half of the page, in degrees anticlockwise on
/// screen (rows grow downward, so a rising line has a negative row slope).
double measuredAngle( const std::vector< float >& out, int W, int H, int& span )
{
	const int c0 = W / 4, c1 = W - W / 4;
	double sx = 0, sy = 0, sxx = 0, sxy = 0;
	int n = 0;
	for( int c = c0; c < c1; ++c )
	{
		double mass = 0.0, moment = 0.0;
		for( int r = 0; r < H; ++r )
		{
			const double d = densityAt( out, W, r, c );
			mass += d;
			moment += d * ( r + 0.5 );
		}
		if( mass <= 0.5 )
			continue;
		const double y = moment / mass;
		sx += c;
		sy += y;
		sxx += static_cast< double >( c ) * c;
		sxy += c * y;
		++n;
	}
	span = n;
	if( n < 2 )
		return 0.0;
	const double slope = ( n * sxy - sx * sy ) / ( n * sxx - sx * sx );
	return -std::atan( slope ) * 180.0 / M_PI;
}

int runSkew( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== skew at %dx%d: the accumulated rotation is the sum of the skews\n", W, H );
	int failed = 0;
	const float slider = 0.75f;
	const double thetaMax = statedSkewDeg( slider );

	Picture pic = flat( W, H, 1.0 );
	paint( pic, W, H, 0, H / 2 - 1, W, H / 2 + 2, 0.0 );

	const int counts[] = { 2, 5, 8 };
	for( int n : counts )
	{
		Knobs k;
		k.skew        = slider;
		k.generations = n;
		std::vector< float > out;
		if( !renderOnce( k, perturb, W, H, pic, out ) )
			return report( false, quiet, "render failed" );
		int span = 0;
		const double got = measuredAngle( out, W, H, span );
		double want      = 0.0;
		for( int g = 1; g <= n; ++g )
			want += thetaMax * statedSkewFraction( g );
		//Tolerance: each column's centroid is within half a pixel of the
		//line's centre (a hard staircase at worst). A deviation bounded by
		//+-1/2 over `span` columns moves a least-squares slope by at most
		//1.5 / span (the worst case is +1/2 on one half and -1/2 on the
		//other: 0.5 ( n^2 / 4 ) / ( n^3 / 12 )), so atan( 1.5 / span ).
		const double tol = std::atan( 1.5 / std::max( 1, span ) ) * 180.0 / M_PI;
		failed += report( span > W / 4 && std::fabs( got - want ) <= tol, quiet, "%d generation%s: measured %+.4f deg, the sum of the skews %+.4f (tolerance %.4f over %d columns)",
		                  n, n == 1 ? "" : "s", got, want, tol, span );
		//Is the negative control (one skew for all) distinguishable here?
		const double same = n * thetaMax * statedSkewFraction( 1 );
		if( n > 1 && !quiet && std::fabs( same - want ) <= 3.0 * tol )
			std::printf( "        (note: reusing skew 1 would give %+.4f, within 3 tolerances -- this case cannot tell the negative control)\n", same );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --ae: grey paper comes out paper white.
//---------------------------------------------------------------------------
int runAE( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== ae at %dx%d: the background is paper white, the patch where T( patch / paper ) says\n", W, H );
	int failed = 0;
	const double paper = 0.55, patches[ 2 ] = { 0.30, 0.42 };
	Picture pic = flat( W, H, paper );
	//Text: horizontal bars, a fifth of the page; two grey patches, a
	//twentieth each, one dark enough to print at full suppression and one
	//light enough to print at half.
	for( int y = H / 10; y < H; y += H / 5 )
		paint( pic, W, H, W / 10, y, W - W / 10, y + H / 25, 0.0 );
	const int py0 = H / 2 + H / 40, py1 = py0 + H / 12;
	const int px0[ 2 ] = { W / 4 - W / 12, 3 * W / 4 - W / 12 }, px1[ 2 ] = { W / 4 + W / 12, 3 * W / 4 + W / 12 };
	for( int i = 0; i < 2; ++i )
		paint( pic, W, H, px0[ i ], py0, px1[ i ], py1, patches[ i ] );

	double lo = 0, hi = 1;
	statedWindow( statedSlope( 0.5f ), lo, hi );
	const float flo = static_cast< float >( lo ), fhi = static_cast< float >( hi );

	const float suppressions[] = { 1.0f, 0.5f };
	for( float s : suppressions )
	{
		Knobs k;
		k.suppression = s;
		std::vector< float > out;
		if( !renderOnce( k, perturb, W, H, pic, out ) )
			return report( false, quiet, "render failed" );

		const double Bm         = asShader( 1.0 * ( 1.0 - s ) + paper * s );
		const double wantPaper  = statedTone( std::min( 1.0, paper / Bm ), flo, fhi );
		//The paper, well away from anything: the top-left tenth.
		double worstPaper = 0.0;
		for( int r = 2; r < H / 10 - 2; ++r )
			for( int c = 2; c < W / 10 - 2; ++c )
				worstPaper = std::max( worstPaper, std::fabs( at( out, W, r, c ) - wantPaper ) );
		//Tolerance: the reading B is a float mean of 144 samples of the
		//paper's float value (144 roundings), the division and the mix two
		//more, the exp three ulp, all through the window and the curve's
		//slope at the patch (at most S).
		const double S   = statedSlope( 0.5f );
		const double tol = ( 150.0 * 2.0 * kU * S ) / ( fhi - flo ) + 3.0 * 2.0 * kU / ( fhi - flo ) + 4.0 * kU;
		failed += report( worstPaper <= tol, quiet, "suppression %.1f: paper %.2f comes out %.6f (want %.6f, worst error %.2e, tolerance %.2e)", s, paper, at( out, W, 3, 3 ), wantPaper, worstPaper, tol );
		for( int i = 0; i < 2; ++i )
		{
			const double wantPatch = statedTone( std::min( 1.0, patches[ i ] / Bm ), flo, fhi );
			double worstPatch      = 0.0;
			for( int r = py0 + 2; r < py1 - 2; ++r )
				for( int c = px0[ i ] + 2; c < px1[ i ] - 2; ++c )
					worstPatch = std::max( worstPatch, std::fabs( at( out, W, r, c ) - wantPatch ) );
			failed += report( worstPatch <= tol, quiet, "  the %.2f patch comes out %.6f (want T( %.4f ) = %.6f, worst error %.2e)", patches[ i ], at( out, W, py0 + 3, px0[ i ] + 3 ), patches[ i ] / Bm, wantPatch, worstPatch );
		}
		if( s == 1.0f )
			failed += report( wantPaper == 1.0 && at( out, W, 3, 3 ) == 1.0f, quiet, "  at full suppression the background is exactly white" );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --negative: every check can fail.
//---------------------------------------------------------------------------
int runNegative( int W, int H )
{
	std::printf( "== negative controls at %dx%d: each perturbation makes its check FAIL\n", W, H );
	struct Control
	{
		const char* what;
		int perturb;
		int ( *check )( int, int, int, bool );
		const char* checkName;
	};
	const Control controls[] = {
		{ "the field kernel replaced by a plain threshold", model::kPerturbNoFringe, runFringe, "--fringe" },
		{ "one skew reused for every generation", model::kPerturbSameSkew, runSkew, "--skew" },
		{ "the AE sensor reading the page mean", model::kPerturbAEMean, runAE, "--ae" },
		{ "the developer depleting by the demand, not what it laid", model::kPerturbStarveDemand, runStarvation, "--starvation" },
		{ "the drum's repeats spaced 1.01 circumferences", model::kPerturbDrumDrift, runDrum, "--drum" },
		{ "a linear photoconductor", model::kPerturbLinearTone, runFixedPoint, "--fixedpoint" },
	};
	int failed = 0;
	for( const Control& c : controls )
	{
		const int before  = g_checks;
		const int failsBefore = g_failures;
		const int caught  = c.check( W, H, c.perturb, true );
		//The quiet run's own bookkeeping is undone: only the verdict counts.
		g_checks   = before;
		g_failures = failsBefore;
		failed += report( caught > 0, false, "%s: %s fails (%d of its assertions)", c.what, c.checkName, caught );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --laws: every control law against its statement. No GL.
//---------------------------------------------------------------------------
int runLaws()
{
	std::printf( "== laws: every control law against its statement (no GL)\n" );
	namespace ctl = tonerfx::controls;
	int failed    = 0;
	double worst  = 0.0;
	for( int i = 0; i <= 20; ++i )
	{
		const float v = static_cast< float >( i ) / 20.0f;
		auto rel      = [ & ]( double a, double b ) { return std::fabs( a - b ) / std::max( 1e-12, std::fabs( b ) ); };
		worst         = std::max( worst, rel( ctl::ContrastSlope( v ), statedSlope( v ) ) );
		worst         = std::max( worst, rel( ctl::OpticsSigma( v ), statedSigma( v ) ) );
		worst         = std::max( worst, rel( ctl::Zoom( v ), statedZoom( v ) ) );
		worst         = std::max( worst, rel( ctl::SkewDegrees( v ), statedSkewDeg( v ) ) );
		worst         = std::max( worst, rel( ctl::OffsetFraction( v ), statedOffsetFraction( v ) ) );
		worst         = std::max( worst, rel( ctl::DepletionPerPage( v ), statedDepletion( v ) ) );
		worst         = std::max( worst, rel( ctl::RecoveryPerPage( v ), statedRecovery( v ) ) );
		worst         = std::max( worst, rel( ctl::ScreenPitchPx( v ), statedPitch( v ) ) );
		worst         = std::max( worst, rel( ctl::CircumferenceFraction( v ), statedCircumference( v ) ) );
		worst         = std::max( worst, rel( ctl::BgSuppression( v ), unit( v ) ) );
		worst         = std::max( worst, rel( ctl::SolidFill( v ), unit( v ) ) );
		worst         = std::max( worst, rel( ctl::DrumMarks( v ), unit( v ) ) );
		worst         = std::max( worst, rel( ctl::ScreenAngleDegrees( v ), 180.0 * unit( v ) ) );
	}
	failed += report( worst <= 1e-12, false, "13 laws at 21 points: worst relative difference %.1e", worst );
	failed += report( ctl::Zoom( 0.5f ) == 1.0, false, "Zoom at 0.5 is exactly 1: %.17g", ctl::Zoom( 0.5f ) );
	failed += report( ctl::DepletionPerPage( 1.0f ) == 0.0 && ctl::RecoveryPerPage( 0.0f ) == 0.0, false, "Toner Supply 1 depletes nothing; Recovery 0 recovers nothing" );
	failed += report( ctl::Generations( 0.4f ) == 1 && ctl::Generations( 3.6f ) == 4 && ctl::Generations( 99.0f ) == 8, false, "Generations rounds and clamps to 1..8" );
	failed += report( ctl::OptionIndex( 6.4f, 7 ) == 6 && ctl::OptionIndex( -1.0f, 7 ) == 0 && ctl::OptionIndex( 3.5f, 4 ) == 3, false, "options map by index" );
	//The model's own promises.
	double lo = 0, hi = 1;
	model::DevelopmentWindow( statedSlope( 0.0f ), lo, hi );
	failed += report( lo >= 0.0 && hi <= 1.0, false, "the development window at the lowest Contrast is inside 0..1: %.4f..%.4f (slope %.3f, floor %.3f)", lo, hi, statedSlope( 0.0f ), model::MinSlope() );
	model::DevelopmentWindow( statedSlope( 1.0f ), lo, hi );
	failed += report( lo >= 0.0 && hi <= 1.0, false, "and at the highest: %.4f..%.4f", lo, hi );
	failed += report( std::fabs( model::Charge( 0.0 ) - 1.0 ) < 1e-15 && std::fabs( model::Charge( 1.0 ) ) < 1e-15, false, "the charge is 1 at black and 0 at paper" );
	const double T = 1.0 - std::clamp( ( model::Charge( 0.5 ) - lo ) / ( hi - lo ), 0.0, 1.0 );
	failed += report( std::fabs( T - 0.5 ) < 1e-12, false, "mid-grey copies as itself: T( 0.5 ) = %.12f", T );
	double mass = 0.0;
	for( int j = -kR; j <= kR; ++j )
		mass += model::KernelTexel( j, model::kGapPx );
	failed += report( std::fabs( mass - 1.0 ) < 1e-12 && std::fabs( model::FieldGain() - statedGain() ) < 1e-9, false, "the truncated kernel sums to 1 (%.15f); the gain is %.4f", mass, model::FieldGain() );
	return failed;
}

//---------------------------------------------------------------------------
// --names: what a host will show, and what oxbow will read.
//---------------------------------------------------------------------------
int runNames()
{
	std::printf( "== names: nothing the host silently truncates (no GL)\n" );
	int failed = 0;
	Toner plugin;
	std::set< std::string > seen;
	int longest = 0;
	std::string longestName;
	bool unique = true;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		if( !seen.insert( p.name ).second )
		{
			unique = false;
			std::printf( "        duplicate: %s\n", p.name.c_str() );
		}
		if( static_cast< int >( p.name.size() ) > longest )
		{
			longest     = static_cast< int >( p.name.size() );
			longestName = p.name;
		}
	}
	failed += report( longest <= 16, false, "longest parameter name is %d characters (%s); the limit is 16", longest, longestName.c_str() );
	failed += report( unique, false, "every parameter name is unique (%zu of them)", seen.size() );
	failed += report( std::strlen( "SW Toner" ) <= 16, false, "the plugin name 'SW Toner' fits the 16-character field" );
	failed += report( std::strlen( "TO01" ) == 4, false, "the id 'TO01' is four characters" );
	return failed;
}

//---------------------------------------------------------------------------
// The moving card, for --out, the sweep, the bench and a default --pipe:
// light-grey paper (so AE has something to do), a photograph-like gradient
// panel, a big solid, medium solids, thin rules, small text-like marks and
// a moving bar.
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	const double t    = static_cast< double >( frame ) / 60.0;
	const double barX = std::fmod( 40.0 + 200.0 * t, static_cast< double >( width ) );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double fx = ( x + 0.5 ) / width, fy = ( y + 0.5 ) / height;
			double v = 0.78;//the paper
			//A photograph: a radial gradient panel, top left.
			if( fx > 0.04 && fx < 0.46 && fy > 0.06 && fy < 0.48 )
			{
				const double dx = ( fx - 0.25 ) / 0.21, dy = ( fy - 0.27 ) / 0.21;
				v = std::clamp( 0.15 + 0.7 * std::sqrt( dx * dx + dy * dy ), 0.0, 1.0 );
			}
			//A big solid, top right.
			if( fx > 0.55 && fx < 0.95 && fy > 0.08 && fy < 0.42 )
				v = 0.05;
			//Medium solids, a row of squares of falling size.
			for( int i = 0; i < 5; ++i )
			{
				const double s  = 0.09 - 0.016 * i;
				const double x0 = 0.06 + 0.19 * i;
				if( fx > x0 && fx < x0 + s * height / width && fy > 0.55 && fy < 0.55 + s )
					v = 0.0;
			}
			//Thin rules, and text-like marks.
			if( fy > 0.72 && fy < 0.74 )
				v = 0.0;
			if( fy > 0.78 && fy < 0.94 && std::fmod( fx * 40.0, 1.0 ) < 0.35 && std::fmod( fy * 60.0, 1.0 ) < 0.6 && ( ( x / 9 + y / 7 ) % 3 ) != 0 )
				v = 0.1;
			//A moving bar.
			if( std::fabs( x + 0.5 - barX ) < std::max( 2.0, width / 160.0 ) )
				v = 0.0;
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< unsigned char >( std::lround( 255.0 * v ) );
			px[ 3 ] = 255;
		}
	return img;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( const std::vector< std::string >& settings, int width, int height, int frames )
{
	Session session;
	session.floatOutput = false;
	for( const std::string& setting : settings )
	{
		std::string error;
		applySetting( session.plugin, setting, error );
	}
	if( !session.begin( width, height ) )
		return -1.0;

	std::vector< std::vector< unsigned char > > loop;
	for( int i = 0; i < 4; ++i )
		loop.push_back( buildCard( width, height, i * 7 ) );

	const int warmup = 10;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( loop[ static_cast< size_t >( frame ) % loop.size() ] );
	glFinish();

	//Best of three: the GPU is shared with other builds on this machine.
	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int i = 0; i < frames; ++i )
			session.renderNow();
		glFinish();
		const double seconds = std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count();
		best                 = std::min( best, seconds * 1000.0 / frames );
	}
	session.end();
	return best;
}

int runBench( const std::vector< std::string >& settings, int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "1280x720  ", 1280, 720 }, { "1920x1080 ", 1920, 1080 }, { "3840x2160 ", 3840, 2160 } };
	std::printf( "%d frames each, best of three runs, after a 10-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame at Generations 1 / 5 / 8      %% of a 60fps frame\n" );
	for( const Size& size : sizes )
	{
		double ms[ 3 ] = { 0, 0, 0 };
		const int gens[ 3 ] = { 1, 5, 8 };
		for( int i = 0; i < 3; ++i )
		{
			std::vector< std::string > s = settings;
			s.push_back( "Generations=" + std::to_string( gens[ i ] ) );
			ms[ i ] = benchAt( s, size.width, size.height, frames );
		}
		std::printf( "%s    %7.2f  %7.2f  %7.2f                 %5.1f%%  %5.1f%%  %5.1f%%\n", size.name, ms[ 0 ], ms[ 1 ], ms[ 2 ],
		             ms[ 0 ] / 16.667 * 100.0, ms[ 1 ] / 16.667 * 100.0, ms[ 2 ] / 16.667 * 100.0 );
	}
	std::printf( "\nEach generation: eleven passes, of which the two 49-tap field passes are most of the cost.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace sh = tonerfx::shaders;
	const std::pair< const char*, const char* > files[] = {
		{ "vertex.vert", sh::kVertex },       { "intake.frag", sh::kIntake },     { "place.frag", sh::kPlace },
		{ "blur.frag", sh::kBlur },           { "ae.frag", sh::kAE },             { "latent.frag", sh::kLatent },
		{ "fieldx.frag", sh::kFieldX },       { "develop.frag", sh::kDevelop },   { "coverage.frag", sh::kCoverage },
		{ "supply.frag", sh::kSupply },       { "marks.frag", sh::kMarks },       { "fuse.frag", sh::kFuse },
		{ "composite.frag", sh::kComposite },
	};
	static_assert( sizeof( files ) / sizeof( files[ 0 ] ) == sh::kFragmentCount + 1, "the dump lists every shader" );
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"totest -- render and measure the Toner photocopier\n"
		"\n"
		"  --out PATH          render the moving card through the plugin (default /tmp/toner.png)\n"
		"  --size WxH          raster (default 1280x720); --width N / --height N also accepted\n"
		"  --frames N          frames to render before reading back (default 40)\n"
		"  --fps N             accepted for the fleet's --pipe contract; this plugin has no clock\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options,\n"
		"                      the integer for Generations). Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size:\n"
		"  --fixedpoint        a ramp copied n times is the tone curve composed n times: two plateaus, a narrowing transition\n"
		"  --fringe            a solid's centre/edge ratio falls with width as the strip field says; a 1-px line prints full\n"
		"  --drum              the drum's repeats are one circumference apart, whole-pixel and fractional\n"
		"  --starvation        density down a page of constant coverage follows the depletion-recovery closed form\n"
		"  --skew              after n generations a line has turned by the sum of the per-generation skews\n"
		"  --ae                grey paper comes out white; a grey patch lands at T( patch / paper )\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed copier (bits in Model.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --laws              every control law against its statement; the model's promises\n"
		"  --names             nothing the host will silently truncate; the host reads SW Toner / TO01\n"
		"  --offline           both; says loudly what it skipped. For CI.\n"
		"  --allow-no-gl       with the rendering checks: SKIP loudly, not FAIL, when no GL 4.1 context exists\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K, Generations 1, 5 and 8\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/toner.png";
	std::string scriptPath;
	std::string dumpDir;
	int width      = 1280;
	int height     = 720;
	int frames     = 40;
	int failRender = -1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	const std::set< std::string > rendered = { "--fixedpoint", "--fringe", "--drum", "--starvation", "--skew", "--ae", "--negative" };
	const std::set< std::string > offline  = { "--laws", "--names" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( argument == "--offline" )
			for( const char* m : { "--laws", "--names" } )
				checks.push_back( m );
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Toner plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		bool needGL     = false;
		bool offlineRan = false;
		for( const std::string& check : checks )
		{
			if( check == "--laws" )
				runLaws();
			else if( check == "--names" )
				runNames();
			else
			{
				needGL = true;
				continue;
			}
			offlineRan = true;
			std::printf( "\n" );
		}
		if( offlineRan && !needGL )
			std::printf( "   OFFLINE: --fixedpoint, --fringe, --drum, --starvation, --skew, --ae and their\n"
			             "   negative controls were NOT run. Nothing here drew a pixel through a GL driver;\n"
			             "   the shaders were not exercised, only (in CI) compiled by glslc. The laws have\n"
			             "   no negative control of their own.\n\n" );

		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const std::string& check : checks )
				{
					if( check == "--fixedpoint" )
						runFixedPoint( width, height, perturb );
					else if( check == "--fringe" )
						runFringe( width, height, perturb );
					else if( check == "--drum" )
						runDrum( width, height, perturb );
					else if( check == "--starvation" )
						runStarvation( width, height, perturb );
					else if( check == "--skew" )
						runSkew( width, height, perturb );
					else if( check == "--ae" )
						runAE( width, height, perturb );
					else if( check == "--negative" )
						runNegative( width, height );
					else
						continue;
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( wantBench )
		return finish( runBench( settings, frames < 40 ? 60 : frames ) );

	Session session;
	session.floatOutput = false;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}
	session.plugin.SetPerturbForTest( perturb );

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		std::map< unsigned int, Track > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation[ static_cast< unsigned int >( index ) ] = entry.second;
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider would.
			for( const auto& track : automation )
				session.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

			const bool rendered = index != failRender && session.render( frame );
			if( !rendered )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	for( int frame = 0; frame < frames; ++frame )
		if( !session.render( buildCard( width, height, frame ) ) )
			return finish( 1 );

	const std::vector< unsigned char > image = session.readBack();
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
