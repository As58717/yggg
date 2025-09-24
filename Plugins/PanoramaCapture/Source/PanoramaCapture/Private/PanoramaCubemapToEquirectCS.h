#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

class FPanoCubemapToEquirectCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FPanoCubemapToEquirectCS);
    SHADER_USE_PARAMETER_STRUCT(FPanoCubemapToEquirectCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_ARRAY(FMatrix44f, ViewMatrices, [6])
        SHADER_PARAMETER(FVector2f, OutputResolution)
        SHADER_PARAMETER(FVector2f, InvOutputResolution)
        SHADER_PARAMETER(FVector2f, FullResolution)
        SHADER_PARAMETER(FVector2f, OutputOffset)
        SHADER_PARAMETER(float, bLinearColorSpace)
        SHADER_PARAMETER_RDG_TEXTURE_SRV_ARRAY(Texture2D<float4>, FaceTextures, [6])
        SHADER_PARAMETER_SAMPLER(SamplerState, FaceSampler)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
    END_SHADER_PARAMETER_STRUCT()

public:
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return Parameters.Platform == SP_PCD3D_SM5 || Parameters.Platform == SP_PCD3D_SM6;
    }
};
