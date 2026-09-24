#pragma once

#include "PassBuffer.h"

#include <FFGLSDK.h>

#include <cstdint>
#include <string>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

/**
	Toner -- a photocopier, and a copy of a copy of a copy, as an FFGL
	effect.

	**The one idea.** A photocopier is a xerographic engine and each stage
	leaves its mark: optics that blur, an auto-exposure that throws the
	background away, a photoconductor whose tone curve is steep,
	development that follows the electric FIELD above the latent image and
	not the image itself, and a toner supply that runs short down the page.
	Copy the copy and the same machine runs again on its own output. Run N
	generations a frame, each with its own seeded skew and offset, and the
	look of zine and punk-flyer art is the fixed point of that loop: greys
	collapse to paper or toner, solids go hollow because a wide charged
	area's field is strongest at its edges, thin lines print full, density
	falls down a heavy page, drum marks repeat every circumference, and the
	skews drift.

	Nothing carries across frames: every frame is copied from its own
	source, the skews and the drum's defects are seeded, so a frame is the
	same picture every time. See AGENTS.md.
*/
class Toner : public CFFGLPlugin
{
public:
	Toner();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- test hooks. Read by totest; the plugin's own operation never uses
	//--- them, and the perturbation is always 0 outside the harness.

	/// Negative-control hooks, a bitmask of `model::Perturb`.
	void SetPerturbForTest( int bits )
	{
		perturb = bits;
	}

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Machine
		PT_GENERATIONS,
		PT_CONTRAST,
		PT_BG_SUPPRESSION,
		PT_OPTICS,
		PT_ZOOM,
		PT_SKEW,

		//Development
		PT_SOLID_FILL,
		PT_TONER_SUPPLY,
		PT_RECOVERY,

		//Photo
		PT_PHOTO_MODE,
		PT_SCREEN,
		PT_SCREEN_ANGLE,

		//Page
		PT_PAPER,
		PT_DRUM_MARKS,
		PT_CIRCUMFERENCE,
		PT_DIRECTION,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	ffglex::FFGLShader intakeShader;
	ffglex::FFGLShader placeShader;
	ffglex::FFGLShader blurShader;
	ffglex::FFGLShader aeShader;
	ffglex::FFGLShader latentShader;
	ffglex::FFGLShader fieldXShader;
	ffglex::FFGLShader developShader;
	ffglex::FFGLShader coverageShader;
	ffglex::FFGLShader supplyShader;
	ffglex::FFGLShader marksShader;
	ffglex::FFGLShader fuseShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLScreenQuad quad;

	tonerfx::PassBuffer pages[ 2 ];///< the page in and the page out, R32F, ping-ponged per generation
	tonerfx::PassBuffer work[ 2 ]; ///< the stages between, R32F
	tonerfx::PassBuffer field;     ///< the x pass of both kernels, RG32F
	tonerfx::PassBuffer ae;        ///< 1 x 1: the AE reading
	tonerfx::PassBuffer coverage;  ///< L x 1: each process row's mean density
	tonerfx::PassBuffer supply;    ///< L x 1: the developer's concentration per row

	int perturb = 0;

	/// Zero-initialised: the About block's ids are never stored to.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
