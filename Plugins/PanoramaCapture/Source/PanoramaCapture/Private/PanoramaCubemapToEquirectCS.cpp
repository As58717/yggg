#include "PanoramaCubemapToEquirectCS.h"

#include "RenderGraph.h"
#include "ShaderCompilerCore.h"

IMPLEMENT_GLOBAL_SHADER(FPanoCubemapToEquirectCS, "/PanoramaCapture/PanoramaCubemapToEquirect.usf", "Main", SF_Compute);
