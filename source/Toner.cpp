#include "Toner.h"

#include "Controls.h"
#include "Diag.h"
#include "Model.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9). The symptom without it is an unknown-type error on
//ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace ffglex;
using namespace tonerfx;

static_assert( model::kKernelRadius == 16, "the field shaders declare 33 taps" );

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Toner >,// Create method
	"TO01",                // Plugin unique ID of maximum length 4.
	"SW Toner",            // Plugin name
	2,                     // API major version number
	1,                     // API minor version number
	0,                     // Plugin major version number
	1,                     // Plugin minor version number
	FF_EFFECT,             // Plugin type
	"A photocopier, and a copy of a copy of a copy.\n\nEach generation is the whole xerographic engine run on the last one's page: optics that blur, an auto-exposure that throws the background away, a steep photoconductor, development that follows the electric field so solids go hollow and thin lines print full, a toner supply that runs short down the page, drum marks that repeat every circumference, and a skew per copy.\n\nStart with Generations and Contrast; Solid Fill brings the middles of solids back.",// Plugin description
	"Toner FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr with no current context; a log line must never
/// be the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

GLint loc( const FFGLShader& shader, const char* name )
{
	return glGetUniformLocation( shader.GetGLID(), name );
}

/// A pass: the target bound and sized, the shader bound, each texture on
/// its unit. Raw binds -- nothing scoped survives eleven passes a
/// generation, and the composite rebinds everything it needs.
void bindTarget( PassBuffer& target )
{
	glBindFramebuffer( GL_FRAMEBUFFER, target.GetGLID() );
	target.ResizeViewPort();
}

void bindTextures( std::initializer_list< GLuint > textures )
{
	int unit = 0;
	for( GLuint texture : textures )
	{
		glActiveTexture( GL_TEXTURE0 + unit );
		glBindTexture( GL_TEXTURE_2D, texture );
		++unit;
	}
	glActiveTexture( GL_TEXTURE0 );
}

void unbindTextures( int count )
{
	for( int unit = count - 1; unit >= 0; --unit )
	{
		glActiveTexture( GL_TEXTURE0 + unit );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	glActiveTexture( GL_TEXTURE0 );
}
} // namespace

//---------------------------------------------------------------------------
Toner::Toner()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//No clock: nothing here moves with time. Each generation's skew and the
	//drum's defects are seeded by index, so a frame is the same picture in
	//the harness as in a host and every time it is rendered.
	SetTimeSupported( false );

	//---------------------------------------------------------------------
	// Defaults. Filled BEFORE any declaration: SetParamInfof reads its
	// default out of GetFloatParameter (compander's trap).
	//---------------------------------------------------------------------
	params[ PT_GENERATIONS ]    = 3.0f;
	params[ PT_CONTRAST ]       = 0.5f; //slope 5.1 at mid-grey
	params[ PT_BG_SUPPRESSION ] = 0.6f;
	params[ PT_OPTICS ]         = 0.5f; //sigma 1 px
	params[ PT_ZOOM ]           = 0.5f; //exactly 100%
	params[ PT_SKEW ]           = 0.3f; //0.6 degrees
	params[ PT_SOLID_FILL ]     = 0.5f; //above the window's top: pure black stays solid
	params[ PT_TONER_SUPPLY ]   = 0.85f;//a black page keeps its toner; lower and it runs out part-way down
	params[ PT_RECOVERY ]       = 0.5f;
	params[ PT_PHOTO_MODE ]     = 0.0f;
	params[ PT_SCREEN ]         = 0.4f; //7 px cells
	params[ PT_SCREEN_ANGLE ]   = 0.25f;//45 degrees
	params[ PT_PAPER ]          = static_cast< float >( model::kWhite );
	params[ PT_DRUM_MARKS ]     = 0.3f;
	params[ PT_CIRCUMFERENCE ]  = 0.25f;//0.325 of the page
	params[ PT_DIRECTION ]      = static_cast< float >( model::kDown );
	params[ PT_MIX ]            = 1.0f;

	//A real integer with a real range: FF_TYPE_INTEGER is exempt from the
	//0..1 clamp of a STANDARD default.
	SetParamInfo( PT_GENERATIONS, "Generations", FF_TYPE_INTEGER, params[ PT_GENERATIONS ] );
	SetParamRange( PT_GENERATIONS, static_cast< float >( model::kGenerationsMin ), static_cast< float >( model::kGenerationsMax ) );
	SetParamInfof( PT_CONTRAST, "Contrast", FF_TYPE_STANDARD );
	//"Background Suppression" is 22 characters; a host shows 16.
	SetParamInfof( PT_BG_SUPPRESSION, "Bg Suppression", FF_TYPE_STANDARD );
	SetParamInfof( PT_OPTICS, "Optics", FF_TYPE_STANDARD );
	SetParamInfof( PT_ZOOM, "Zoom", FF_TYPE_STANDARD );
	SetParamInfof( PT_SKEW, "Skew", FF_TYPE_STANDARD );

	SetParamInfof( PT_SOLID_FILL, "Solid Fill", FF_TYPE_STANDARD );
	SetParamInfof( PT_TONER_SUPPLY, "Toner Supply", FF_TYPE_STANDARD );
	SetParamInfof( PT_RECOVERY, "Recovery", FF_TYPE_STANDARD );

	SetParamInfo( PT_PHOTO_MODE, "Photo Mode", FF_TYPE_BOOLEAN, false );
	SetParamInfof( PT_SCREEN, "Screen", FF_TYPE_STANDARD );
	SetParamInfof( PT_SCREEN_ANGLE, "Screen Angle", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_PAPER, "Paper", model::kPaperCount, params[ PT_PAPER ] );
	for( int i = 0; i < model::kPaperCount; ++i )
		SetParamElementInfo( PT_PAPER, static_cast< unsigned int >( i ), model::kPaperNames[ i ], static_cast< float >( i ) );
	SetParamInfof( PT_DRUM_MARKS, "Drum Marks", FF_TYPE_STANDARD );
	//"Drum Circumference" is 18 characters.
	SetParamInfof( PT_CIRCUMFERENCE, "Circumference", FF_TYPE_STANDARD );
	SetOptionParamInfo( PT_DIRECTION, "Direction", model::kDirectionCount, params[ PT_DIRECTION ] );
	for( int i = 0; i < model::kDirectionCount; ++i )
		SetParamElementInfo( PT_DIRECTION, static_cast< unsigned int >( i ), model::kDirectionNames[ i ], static_cast< float >( i ) );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//SetParamGroup collapses consecutive ids under one header, so each group
	//is a contiguous run of the enum.
	for( FFUInt32 i = PT_GENERATIONS; i <= PT_SKEW; ++i )
		SetParamGroup( i, "Machine" );
	for( FFUInt32 i = PT_SOLID_FILL; i <= PT_RECOVERY; ++i )
		SetParamGroup( i, "Development" );
	for( FFUInt32 i = PT_PHOTO_MODE; i <= PT_SCREEN_ANGLE; ++i )
		SetParamGroup( i, "Photo" );
	for( FFUInt32 i = PT_PAPER; i <= PT_MIX; ++i )
		SetParamGroup( i, "Page" );

	// The About block. Declared inline: SetParamInfo is protected on
	// CFFGLPlugin and nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Toner effect" );
	diag::init();
}

//---------------------------------------------------------------------------
FFResult Toner::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &intakeShader, shaders::kIntake, "intake" },
		{ &placeShader, shaders::kPlace, "place" },
		{ &blurShader, shaders::kBlur, "blur" },
		{ &aeShader, shaders::kAE, "ae" },
		{ &latentShader, shaders::kLatent, "latent" },
		{ &fieldXShader, shaders::kFieldX, "fieldx" },
		{ &developShader, shaders::kDevelop, "develop" },
		{ &coverageShader, shaders::kCoverage, "coverage" },
		{ &supplyShader, shaders::kSupply, "supply" },
		{ &marksShader, shaders::kMarks, "marks" },
		{ &fuseShader, shaders::kFuse, "fuse" },
		{ &compositeShader, shaders::kComposite, "composite" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( shaders::kVertex, stage.fragment ) )
			continue;
		//FF_FAIL is invisible to an operator: the effect simply does nothing.
		//This line is the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Toner: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Toner::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	//The host's viewport, before anything of ours changes it:
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	const int W = static_cast< int >( picture.Width );
	const int H = static_cast< int >( picture.Height );

	//---------------------------------------------------------------------
	// The settings, in physical units.
	//---------------------------------------------------------------------
	const int generations   = controls::Generations( params[ PT_GENERATIONS ] );
	const double slope      = controls::ContrastSlope( params[ PT_CONTRAST ] );
	const double suppress   = controls::BgSuppression( params[ PT_BG_SUPPRESSION ] );
	const double sigma      = controls::OpticsSigma( params[ PT_OPTICS ] );
	const double zoom       = controls::Zoom( params[ PT_ZOOM ] );
	const double skewMax    = controls::SkewDegrees( params[ PT_SKEW ] );
	const double offsetAmp  = controls::OffsetFraction( params[ PT_SKEW ] ) * std::min( W, H );
	const double electrode  = controls::SolidFill( params[ PT_SOLID_FILL ] );
	const double depletion  = controls::DepletionPerPage( params[ PT_TONER_SUPPLY ] );
	const double recovery   = controls::RecoveryPerPage( params[ PT_RECOVERY ] );
	const bool photoMode    = params[ PT_PHOTO_MODE ] >= 0.5f;
	const double pitch      = controls::ScreenPitchPx( params[ PT_SCREEN ] );
	const double screenDeg  = controls::ScreenAngleDegrees( params[ PT_SCREEN_ANGLE ] );
	const int paper         = controls::OptionIndex( params[ PT_PAPER ], model::kPaperCount );
	const double marks      = controls::DrumMarks( params[ PT_DRUM_MARKS ] );
	const double circumFrac = controls::CircumferenceFraction( params[ PT_CIRCUMFERENCE ] );
	const int direction     = controls::OptionIndex( params[ PT_DIRECTION ], model::kDirectionCount );

	const int L = direction < model::kRight ? H : W;//along the process direction
	const int A = direction < model::kRight ? W : H;//across it

	//---------------------------------------------------------------------
	// Buffers. Every allocation happens here, before anything binds a
	// texture: FFGLFBO::Initialise sizes its colour texture under a scoped
	// binding, and every ffglex Scoped* binding CLEARS to 0 on exit.
	//---------------------------------------------------------------------
	const auto nearest = PassBuffer::Sampling::Nearest;
	if( !pages[ 0 ].Ensure( W, H, GL_R32F, nearest ) || !pages[ 1 ].Ensure( W, H, GL_R32F, nearest ) || !work[ 0 ].Ensure( W, H, GL_R32F, nearest )
	    || !work[ 1 ].Ensure( W, H, GL_R32F, nearest ) || !field.Ensure( W, H, GL_RG32F, nearest ) || !ae.Ensure( 1, 1, GL_R32F, nearest )
	    || !coverage.Ensure( L, 1, GL_R32F, nearest ) || !supply.Ensure( L, 1, GL_R32F, nearest ) )
	{
		diag::error( "could not allocate the page buffers: " + std::to_string( W ) + " x " + std::to_string( H ) );
		return FF_FAIL;
	}

	//---------------------------------------------------------------------
	// The coefficients, in double, once a frame.
	//---------------------------------------------------------------------
	//The optics: a Gaussian to three sigma, at most 12 either side.
	int blurRadius = sigma > 0.0 ? std::min( 12, static_cast< int >( std::ceil( 3.0 * sigma ) ) ) : 0;
	float blurWeights[ 13 ] = { 1.0f };
	{
		double weights[ 13 ] = { 1.0 };
		double total         = 1.0;
		for( int j = 1; j <= blurRadius; ++j )
		{
			weights[ j ] = std::exp( -( j * j ) / ( 2.0 * sigma * sigma ) );
			total += 2.0 * weights[ j ];
		}
		for( int j = 0; j <= blurRadius; ++j )
			blurWeights[ j ] = static_cast< float >( weights[ j ] / total );
	}

	//The two field kernels and the gain.
	float kernelH[ 2 * model::kKernelRadius + 1 ];
	float kernelHH[ 2 * model::kKernelRadius + 1 ];
	{
		const double farGap = model::kGapPx + 2.0 * model::kThicknessPx;
		for( int j = -model::kKernelRadius; j <= model::kKernelRadius; ++j )
		{
			kernelH[ j + model::kKernelRadius ]  = static_cast< float >( model::KernelTexel( j, model::kGapPx ) );
			kernelHH[ j + model::kKernelRadius ] = static_cast< float >( model::KernelTexel( j, farGap ) );
		}
	}
	const double gain = model::FieldGain();

	//The development window.
	double windowLo = 0.0, windowHi = 1.0;
	model::DevelopmentWindow( slope, windowLo, windowHi );

	//The drum: defects in process pixels, the circumference in pixels.
	const double circumference = circumFrac * L;
	float defectU[ model::kDrumDefects ], defectV[ model::kDrumDefects ], defectSigma[ model::kDrumDefects ];
	int defectSpeck[ model::kDrumDefects ];
	for( int i = 0; i < model::kDrumDefects; ++i )
	{
		const model::Defect d = model::DrumDefect( i );
		defectU[ i ]          = static_cast< float >( d.u * circumference );
		defectV[ i ]          = static_cast< float >( d.v * A );
		defectSigma[ i ]      = static_cast< float >( d.sigma );
		defectSpeck[ i ]      = d.speck ? 1 : 0;
	}

	const double screenRad = screenDeg * model::kPi / 180.0;

	//---------------------------------------------------------------------
	// Intake: the host's picture as page 0.
	//---------------------------------------------------------------------
	{
		bindTarget( pages[ 0 ] );
		ScopedShaderBinding shader( intakeShader.GetGLID() );
		bindTextures( { picture.Handle } );
		intakeShader.Set( "InputTexture", 0 );
		quad.Draw();
		unbindTextures( 1 );
	}

	//---------------------------------------------------------------------
	// The generations.
	//---------------------------------------------------------------------
	for( int g = 1; g <= generations; ++g )
	{
		PassBuffer& pageIn  = pages[ ( g - 1 ) & 1 ];
		PassBuffer& pageOut      = pages[ g & 1 ];

		//This generation's placement. Positive skew turns the page
		//anticlockwise as seen (the picture is upright in GL's frame, so
		//that is the standard rotation); the inverse map applied to output
		//pixels is the rotation by -theta, over the zoom.
		const int seedGen   = ( perturb & model::kPerturbSameSkew ) ? 1 : g;//Perturb 2: one skew for all (a negative control)
		const double theta  = skewMax * model::SkewFraction( seedGen ) * model::kPi / 180.0;
		const double dx     = offsetAmp * model::OffsetXFraction( seedGen );
		const double dy     = offsetAmp * model::OffsetYFraction( seedGen );
		const double c      = std::cos( theta ) / zoom;
		const double s      = std::sin( theta ) / zoom;
		const float inv[ 4 ] = { static_cast< float >( c ), static_cast< float >( s ), static_cast< float >( -s ), static_cast< float >( c ) };

		//1. place
		{
			bindTarget( work[ 0 ] );
			ScopedShaderBinding shader( placeShader.GetGLID() );
			bindTextures( { pageIn.TextureID() } );
			placeShader.Set( "PageTexture", 0 );
			placeShader.Set( "PageW", W );
			placeShader.Set( "PageH", H );
			placeShader.Set( "Inv", inv[ 0 ], inv[ 1 ], inv[ 2 ], inv[ 3 ] );
			placeShader.Set( "Centre", static_cast< float >( W * 0.5 ), static_cast< float >( H * 0.5 ) );
			placeShader.Set( "Offset", static_cast< float >( dx ), static_cast< float >( dy ) );
			quad.Draw();
		}

		//2. optics, x then y: work0 -> work1 -> work0
		for( int axis = 0; axis < 2; ++axis )
		{
			bindTarget( work[ 1 - axis ] );
			ScopedShaderBinding shader( blurShader.GetGLID() );
			bindTextures( { work[ axis ].TextureID() } );
			blurShader.Set( "PageTexture", 0 );
			blurShader.Set( "PageW", W );
			blurShader.Set( "PageH", H );
			blurShader.Set( "Axis", axis );
			blurShader.Set( "Radius", blurRadius );
			glUniform1fv( loc( blurShader, "Weights" ), 13, blurWeights );
			quad.Draw();
		}

		//3. the AE sensor
		{
			bindTarget( ae );
			ScopedShaderBinding shader( aeShader.GetGLID() );
			bindTextures( { work[ 0 ].TextureID() } );
			aeShader.Set( "PageTexture", 0 );
			aeShader.Set( "PageW", W );
			aeShader.Set( "PageH", H );
			aeShader.Set( "AEFloor", static_cast< float >( model::kAEFloor ) );
			aeShader.Set( "Perturb", perturb );
			quad.Draw();
		}

		//4. the latent image: work0 + ae -> work1
		{
			bindTarget( work[ 1 ] );
			ScopedShaderBinding shader( latentShader.GetGLID() );
			bindTextures( { work[ 0 ].TextureID(), ae.TextureID() } );
			latentShader.Set( "PageTexture", 0 );
			latentShader.Set( "AETexture", 1 );
			latentShader.Set( "Suppression", static_cast< float >( suppress ) );
			latentShader.Set( "PhotoMode", photoMode ? 1 : 0 );
			latentShader.Set( "ScreenPitch", static_cast< float >( pitch ) );
			latentShader.Set( "ScreenAxis", static_cast< float >( std::cos( screenRad ) ), static_cast< float >( std::sin( screenRad ) ) );
			latentShader.Set( "Pidc", static_cast< float >( model::kPidc ) );
			latentShader.Set( "PidcFloor", static_cast< float >( std::exp( -model::kPidc ) ) );
			latentShader.Set( "Perturb", perturb );
			quad.Draw();
		}

		//5. the field's x pass: work1 (q) -> field
		{
			bindTarget( field );
			ScopedShaderBinding shader( fieldXShader.GetGLID() );
			bindTextures( { work[ 1 ].TextureID() } );
			fieldXShader.Set( "ChargeTexture", 0 );
			fieldXShader.Set( "PageW", W );
			fieldXShader.Set( "PageH", H );
			fieldXShader.Set( "Radius", model::kKernelRadius );
			glUniform1fv( loc( fieldXShader, "KernelH" ), 2 * model::kKernelRadius + 1, kernelH );
			glUniform1fv( loc( fieldXShader, "KernelHH" ), 2 * model::kKernelRadius + 1, kernelHH );
			quad.Draw();
		}

		//6. development: field + work1 (q) -> work0 (D)
		{
			bindTarget( work[ 0 ] );
			ScopedShaderBinding shader( developShader.GetGLID() );
			bindTextures( { field.TextureID(), work[ 1 ].TextureID() } );
			developShader.Set( "FieldTexture", 0 );
			developShader.Set( "ChargeTexture", 1 );
			developShader.Set( "PageW", W );
			developShader.Set( "PageH", H );
			developShader.Set( "Radius", model::kKernelRadius );
			glUniform1fv( loc( developShader, "KernelH" ), 2 * model::kKernelRadius + 1, kernelH );
			glUniform1fv( loc( developShader, "KernelHH" ), 2 * model::kKernelRadius + 1, kernelHH );
			developShader.Set( "Gain", static_cast< float >( gain ) );
			developShader.Set( "Electrode", static_cast< float >( electrode ) );
			developShader.Set( "WindowLo", static_cast< float >( windowLo ) );
			developShader.Set( "WindowHi", static_cast< float >( windowHi ) );
			developShader.Set( "Perturb", perturb );
			quad.Draw();
		}

		//7. coverage: work0 (D) -> coverage (L x 1)
		{
			bindTarget( coverage );
			ScopedShaderBinding shader( coverageShader.GetGLID() );
			bindTextures( { work[ 0 ].TextureID() } );
			coverageShader.Set( "DensityTexture", 0 );
			coverageShader.Set( "Direction", direction );
			coverageShader.Set( "PageW", W );
			coverageShader.Set( "PageH", H );
			quad.Draw();
		}

		//8. supply: coverage -> supply (L x 1)
		{
			bindTarget( supply );
			ScopedShaderBinding shader( supplyShader.GetGLID() );
			bindTextures( { coverage.TextureID() } );
			supplyShader.Set( "CoverageTexture", 0 );
			supplyShader.Set( "Direction", direction );
			supplyShader.Set( "PageW", W );
			supplyShader.Set( "PageH", H );
			supplyShader.Set( "Recovery", static_cast< float >( controls::PerRow( recovery, L ) ) );
			supplyShader.Set( "Depletion", static_cast< float >( controls::PerRow( depletion, L ) ) );
			supplyShader.Set( "Perturb", perturb );
			quad.Draw();
		}

		//9. starvation and the drum: work0 (D) + supply -> work1 (D')
		{
			bindTarget( work[ 1 ] );
			ScopedShaderBinding shader( marksShader.GetGLID() );
			bindTextures( { work[ 0 ].TextureID(), supply.TextureID() } );
			marksShader.Set( "DensityTexture", 0 );
			marksShader.Set( "SupplyTexture", 1 );
			marksShader.Set( "Direction", direction );
			marksShader.Set( "PageW", W );
			marksShader.Set( "PageH", H );
			marksShader.Set( "Circumference", static_cast< float >( circumference ) );
			marksShader.Set( "Amplitude", static_cast< float >( marks ) );
			glUniform1fv( loc( marksShader, "DefectU" ), model::kDrumDefects, defectU );
			glUniform1fv( loc( marksShader, "DefectV" ), model::kDrumDefects, defectV );
			glUniform1fv( loc( marksShader, "DefectSigma" ), model::kDrumDefects, defectSigma );
			glUniform1iv( loc( marksShader, "DefectSpeck" ), model::kDrumDefects, defectSpeck );
			marksShader.Set( "Perturb", perturb );
			quad.Draw();
		}

		//10. fuse: work1 (D') -> page out
		{
			bindTarget( pageOut );
			ScopedShaderBinding shader( fuseShader.GetGLID() );
			bindTextures( { work[ 1 ].TextureID() } );
			fuseShader.Set( "DensityTexture", 0 );
			fuseShader.Set( "PageW", W );
			fuseShader.Set( "PageH", H );
			fuseShader.Set( "Spread", static_cast< float >( model::kFuseSpread ) );
			quad.Draw();
		}
		unbindTextures( 2 );
	}

	//---------------------------------------------------------------------
	// Composite.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( compositeShader.GetGLID() );
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding input( picture.Handle );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding page( pages[ generations & 1 ].TextureID() );

		compositeShader.Set( "InputTexture", 0 );
		compositeShader.Set( "PageTexture", 1 );
		compositeShader.Set( "PageW", W );
		compositeShader.Set( "PageH", H );
		compositeShader.Set( "Paper", model::kPaperRGB[ paper ][ 0 ], model::kPaperRGB[ paper ][ 1 ], model::kPaperRGB[ paper ][ 2 ] );
		compositeShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Toner::DeInitGL()
{
	for( FFGLShader* shader : { &intakeShader, &placeShader, &blurShader, &aeShader, &latentShader, &fieldXShader, &developShader,
	                            &coverageShader, &supplyShader, &marksShader, &fuseShader, &compositeShader } )
		shader->FreeGLResources();
	quad.Release();

	for( PassBuffer* buffer : { &pages[ 0 ], &pages[ 1 ], &work[ 0 ], &work[ 1 ], &field, &ae, &coverage, &supply } )
		buffer->Destroy();
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Toner::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Toner::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

char* Toner::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Toner::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only; it has to say so
	// successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}
