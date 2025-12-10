#include "FisheyeCS4CameraRendering.h"
#include <cassert>

#include "FileManager.h"
#include "Containers/DynamicRHIResourceArray.h"
#include "Engine/Classes/Engine/TextureRenderTarget2D.h"  
#include "Engine/Classes/Engine/World.h"  
#include "RenderCore/Public/GlobalShader.h"  
#include "RHI/Public/PipelineStateCache.h"  
#include "RHI/Public/RHIStaticStates.h"  
#include "Engine/Public/SceneInterface.h"  
#include "RenderCore/Public/ShaderParameterUtils.h"  
#include "Core/Public/Logging/MessageLog.h"  
#include "Core/Public/Internationalization/Internationalization.h"  
#include "Runtime/Engine/Classes/Engine/Texture2D.h"
#include "Runtime/RenderCore/Public/RenderTargetPool.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"
#include "CoreMinimal.h"
#include "ParallelFor.h"

float EPS = 0.001f;

#pragma optimize("", off)
#define LOCTEXT_NAMESPACE "FisheyeCS4Camera"

TMap<FString, TSharedPtr<TResourceArray<int>>> UFisheyeCS4CameraRendering::MapSamplePanelID;
TMap<FString, FStructuredBufferRHIRef> UFisheyeCS4CameraRendering::MapSamplePanelIDBuffer;
TMap<FString, FShaderResourceViewRHIRef> UFisheyeCS4CameraRendering::MapSamplePanelIDSRV;
TMap<FString, FRHIResourceCreateInfo*> UFisheyeCS4CameraRendering::MapCreateInfoSamplePanelID;

TMap<FString, TSharedPtr<TResourceArray<float>>> UFisheyeCS4CameraRendering::MapFisheyeMask;
TMap<FString, FStructuredBufferRHIRef> UFisheyeCS4CameraRendering::MapFisheyeMaskBuffer;
TMap<FString, FShaderResourceViewRHIRef> UFisheyeCS4CameraRendering::MapFisheyeMaskSRV;
TMap<FString, FRHIResourceCreateInfo*> UFisheyeCS4CameraRendering::MapFisheyeMaskCreateInfo;

// replace . to _dot_
FString EncodeIDToFileName(const FString& ID)
{
    FString Encoded = ID;
    Encoded.ReplaceInline(TEXT("."), TEXT("_dot_"));
    return Encoded;
}

// replace _dot_ to .
FString DecodeFileNameToID(const FString& EncodedFileName)
{
    FString Decoded = EncodedFileName;
    Decoded.ReplaceInline(TEXT("_dot_"), TEXT("."));
    return Decoded;
}

float UFisheyeCS4CameraRendering::GetClampedKernelRadius(uint32 SampleCountMax, float KernelRadius)
{
    return FMath::Clamp<float>(KernelRadius, DELTA, SampleCountMax - 1);
}

int UFisheyeCS4CameraRendering::GetIntegerKernelRadius(uint32 SampleCountMax, float KernelRadius)
{
    // Smallest radius will be 1.
    return FMath::Min<int32>(FMath::CeilToInt(GetClampedKernelRadius(SampleCountMax, KernelRadius)), SampleCountMax - 1);
}

float UFisheyeCS4CameraRendering::NormalDistributionUnscaled(float X, float Sigma)
{
    const float DX = FMath::Abs(X);
    const float Gaussian = FMath::Exp(-16.7f * FMath::Square(DX / Sigma));
    return Gaussian;
}

void UFisheyeCS4CameraRendering::Compute1DGaussianFilterKernel(TResourceArray<float>& Gaussian1dKernel, uint32 SampleCountMax, float KernelRadius)
{
    const float ClampedKernelRadius = GetClampedKernelRadius(SampleCountMax, KernelRadius);
    const int32 IntegerKernelRadius = GetIntegerKernelRadius(SampleCountMax, KernelRadius);
    uint32 SampleCount = 0;
    float WeightSum = 0.0f;

    for (int32 SampleIndex = -IntegerKernelRadius; SampleIndex <= IntegerKernelRadius; SampleIndex++)
    {
        float Weight = NormalDistributionUnscaled(SampleIndex, ClampedKernelRadius);
        Gaussian1dKernel.Add(Weight);
        WeightSum += Weight;
        SampleCount++;
    }

    float WeightSumInverse = 1.0f / WeightSum;
    for (uint32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
    {
        Gaussian1dKernel[SampleIndex] *= WeightSumInverse;
    }
}


// Pack three integer values into a single int32_t
void UFisheyeCS4CameraRendering::PackToInt32(int &res, int texidx2, int miplv2, int x12, int y12, int weight4) {
    assert(texidx2 >= 0 && texidx2 < (1 << 2));
    assert(miplv2 >= 0 && miplv2 < (1 << 2));
    assert(x12 >= 0 && x12 < (1 << 12));
    assert(y12 >= 0 && y12 < (1 << 12));
    assert(weight4 >= 0 && weight4 < (1 << 4));
    res = (texidx2 << 30) | (miplv2 << 28) | (x12 << 16) | (y12 << 4) | weight4;
}

// Unpack three values from a single int32_t
void UFisheyeCS4CameraRendering::UnpackFromInt32(int res, int &texidx2, int &miplv2, int &x12, int &y12, int &weight4) {
    texidx2 = (res >> 30) & 0x3;
    miplv2 = (res >> 28) & 0x3;
    x12 = (res >> 16) & 0xFFF;
    y12 = (res >> 4) & 0xFFF;
    weight4 = res & 0xF;
}

// Pack with texidx using 3 bits, miplv using 1 bit
void UFisheyeCS4CameraRendering::PackToInt32_Tex3Bit(int& res, int texidx3, int miplv1, int x12, int y12, int weight4) {
    assert(texidx3 >= 0 && texidx3 < (1 << 3));  // 0~7
    assert(miplv1 >= 0 && miplv1 < (1 << 1));    // 0~1
    assert(x12 >= 0 && x12 < (1 << 12));         // 0~4095
    assert(y12 >= 0 && y12 < (1 << 12));         // 0~4095
    assert(weight4 >= 0 && weight4 < (1 << 4));  // 0~15

    res = (texidx3 << 29) | (miplv1 << 28) | (x12 << 16) | (y12 << 4) | weight4;
}

// Unpack with texidx using 3 bits, miplv using 1 bit
void UFisheyeCS4CameraRendering::UnpackFromInt32_Tex3Bit(int res, int& texidx3, int& miplv1, int& x12, int& y12, int& weight4) {
    texidx3 = (res >> 29) & 0x7;   // 3 bits
    miplv1 = (res >> 28) & 0x1;   // 1 bit
    x12 = (res >> 16) & 0xFFF; // 12 bits
    y12 = (res >> 4) & 0xFFF; // 12 bits
    weight4 = res & 0xF;           // 4 bits
}


UFisheyeCS4CameraRendering::UFisheyeCS4CameraRendering(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
}

class FFisheyeCS4CameraComputeShader : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FFisheyeCS4CameraComputeShader, Global)

public:
    FFisheyeCS4CameraComputeShader() {}
    FFisheyeCS4CameraComputeShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
        : FGlobalShader(Initializer)
    {
        InputTexture.Bind(Initializer.ParameterMap, TEXT("InputTexture"));
        RWOutputTexture.Bind(Initializer.ParameterMap, TEXT("OutputTexture"));
        SamplePanelID.Bind(Initializer.ParameterMap, TEXT("SamplePanelID"));
        SnitchNum.Bind(Initializer.ParameterMap, TEXT("SnitchNum"));
    }

    void SetParameters(
        FRHICommandListImmediate& RHICmdList,
        TArray<TRefCountPtr<FRHITexture>> InputTextureRef,
        FTextureRHIRef& OutTextureRef,
        FUnorderedAccessViewRHIRef& OutputTextureUAVRef,
        FSamplerStateRHIRef SamplerState,
        FShaderResourceViewRHIRef& SamplePanelIDSRV,
        int32 SnitchTexNum)
    {
        for (int i = 0; i < InputTextureRef.Num(); i++)
        {
            if (InputTextureRef.IsValidIndex(i))
            {
                RHICmdList.SetShaderTexture(GetComputeShader(), InputTexture.GetBaseIndex() + i, InputTextureRef[i]);
            }
            else
            {
                UE_LOG(LogTemp, Error, TEXT("InputTextureRef.IsValidIndex(%d)"), i);
            }
        }
        RWOutputTexture.SetTexture(RHICmdList, GetComputeShader(), OutTextureRef, OutputTextureUAVRef);
        SetShaderValue(RHICmdList, GetComputeShader(), SnitchNum, SnitchTexNum);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), SamplePanelID.GetBaseIndex(), SamplePanelIDSRV);
    }

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    }

    virtual bool Serialize(FArchive& Ar) override
    {
        bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
        Ar << InputTexture;
        Ar << RWOutputTexture;
        Ar << SamplePanelID;
        Ar << SnitchNum;
        return bShaderHasOutdatedParameters;
    }

private:
    FShaderResourceParameter InputTexture;
    FRWShaderParameter RWOutputTexture;
    FShaderResourceParameter SamplePanelID;
    FShaderParameter SnitchNum;
};
IMPLEMENT_SHADER_TYPE(, FFisheyeCS4CameraComputeShader, TEXT("/Plugin/FisheyeCS4Camera/Private/TexturePacker.usf"), TEXT("MainCS"), SF_Compute)

class FMipmapsComputeShader : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FMipmapsComputeShader, Global)

public:
    FMipmapsComputeShader() {}
    FMipmapsComputeShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
        : FGlobalShader(Initializer)
    {
        InputMipmapTexture.Bind(Initializer.ParameterMap, TEXT("InputMipmapTexture"));
        RWOutputMipmapTexture.Bind(Initializer.ParameterMap, TEXT("RWOutputMipmapTexture"));
        InputTextureSampler.Bind(Initializer.ParameterMap, TEXT("InputTextureSampler"));
    }

    void SetParameters(
        FRHICommandListImmediate& RHICmdList,
        FShaderResourceViewRHIRef& InputTextureSRV,
        FUnorderedAccessViewRHIRef& OutputTextureUAV,
        FSamplerStateRHIRef& SamplerState)
    {
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), InputMipmapTexture.GetBaseIndex(), InputTextureSRV);
        RHICmdList.SetUAVParameter(GetComputeShader(), RWOutputMipmapTexture.GetUAVIndex(), OutputTextureUAV);
        RHICmdList.SetShaderSampler(GetComputeShader(), InputTextureSampler.GetBaseIndex(), SamplerState);
    }

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    }

    virtual bool Serialize(FArchive& Ar) override
    {
        bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
        Ar << InputMipmapTexture;
        Ar << RWOutputMipmapTexture;
        Ar << InputTextureSampler;
        return bShaderHasOutdatedParameters;
    }

private:
    FShaderResourceParameter InputMipmapTexture;
    FRWShaderParameter RWOutputMipmapTexture;
    FShaderResourceParameter InputTextureSampler;
};
IMPLEMENT_SHADER_TYPE(, FMipmapsComputeShader, TEXT("/Plugin/FisheyeCS4Camera/Private/GenMipmap.usf"), TEXT("MipmapCS"), SF_Compute)

class FGaussianBlurComputeShader : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FGaussianBlurComputeShader, Global)

public:
    FGaussianBlurComputeShader() {}
    FGaussianBlurComputeShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
        : FGlobalShader(Initializer)
    {
        InputTexture.Bind(Initializer.ParameterMap, TEXT("InputGaussBlurTexture"));
        RWOutputTexture.Bind(Initializer.ParameterMap, TEXT("RWOutputGaussBlurTexture"));
        GaussBlurKernel1d.Bind(Initializer.ParameterMap, TEXT("GaussBlurKernel1d"));
        BlurLength.Bind(Initializer.ParameterMap, TEXT("BlurLength"));
        BlurDirection.Bind(Initializer.ParameterMap, TEXT("BlurDirection"));
        Sampler.Bind(Initializer.ParameterMap, TEXT("Sampler"));
    }

    void SetParameters(
        FRHICommandListImmediate& RHICmdList,
        FShaderResourceViewRHIRef& InputTextureSRV,
        FUnorderedAccessViewRHIRef& OutputTextureUAV,
        FShaderResourceViewRHIRef& GaussBlur1dSRV,
        int32 Length,
        int32 Direction,
        FSamplerStateRHIRef& SamplerState) 
    {
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), InputTexture.GetBaseIndex(), InputTextureSRV);
        RHICmdList.SetUAVParameter(GetComputeShader(), RWOutputTexture.GetUAVIndex(), OutputTextureUAV);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), GaussBlurKernel1d.GetBaseIndex(), GaussBlur1dSRV);
        SetShaderValue(RHICmdList, GetComputeShader(), BlurLength, Length);
        SetShaderValue(RHICmdList, GetComputeShader(), BlurDirection, Direction);
        RHICmdList.SetShaderSampler(GetComputeShader(), Sampler.GetBaseIndex(), SamplerState);
    }

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    }

    virtual bool Serialize(FArchive& Ar) override
    {
        bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
        Ar << InputTexture;
        Ar << RWOutputTexture;
        Ar << GaussBlurKernel1d;
        Ar << BlurLength;
        Ar << BlurDirection;
        Ar << Sampler;
        return bShaderHasOutdatedParameters;
    }

private:
    FShaderResourceParameter InputTexture;
    FRWShaderParameter RWOutputTexture;
    FShaderResourceParameter GaussBlurKernel1d;
    FShaderParameter BlurLength;
    FShaderParameter BlurDirection;
    FShaderResourceParameter Sampler;
};
IMPLEMENT_SHADER_TYPE(, FGaussianBlurComputeShader, TEXT("/Plugin/FisheyeCS4Camera/Private/GaussBlur1d.usf"), TEXT("GaussBlur1dCS"), SF_Compute)

class FGaussianBlurAddComputeShader : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FGaussianBlurAddComputeShader, Global)
public:
    FGaussianBlurAddComputeShader() {}
    FGaussianBlurAddComputeShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
        : FGlobalShader(Initializer)
    {
        InputTexture.Bind(Initializer.ParameterMap, TEXT("InputGaussBlurTexture"));
        InputAddTexture.Bind(Initializer.ParameterMap, TEXT("InputAddTexture"));
        RWOutputTexture.Bind(Initializer.ParameterMap, TEXT("RWOutputGaussBlurTexture"));
        GaussBlurKernel1d.Bind(Initializer.ParameterMap, TEXT("GaussBlurKernel1d"));
        BlurLength.Bind(Initializer.ParameterMap, TEXT("BlurLength"));
        BlurDirection.Bind(Initializer.ParameterMap, TEXT("BlurDirection"));
        Sampler.Bind(Initializer.ParameterMap, TEXT("Sampler"));
        AddSampler.Bind(Initializer.ParameterMap, TEXT("AddSampler"));
    }

    void SetParameters(
        FRHICommandListImmediate& RHICmdList,
        FShaderResourceViewRHIRef& InputTextureSRV,
        FShaderResourceViewRHIRef& InputAddTextureSRV,
        FUnorderedAccessViewRHIRef& OutputTextureUAV,
        FShaderResourceViewRHIRef& GaussBlur1dSRV,
        int32 Length,
        int32 Direction,
        FSamplerStateRHIRef& SamplerState,
        FSamplerStateRHIRef& AddSamplerState)
    {
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), InputTexture.GetBaseIndex(), InputTextureSRV);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), InputAddTexture.GetBaseIndex(), InputAddTextureSRV);
        RHICmdList.SetUAVParameter(GetComputeShader(), RWOutputTexture.GetUAVIndex(), OutputTextureUAV);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), GaussBlurKernel1d.GetBaseIndex(), GaussBlur1dSRV);
        SetShaderValue(RHICmdList, GetComputeShader(), BlurLength, Length);
        SetShaderValue(RHICmdList, GetComputeShader(), BlurDirection, Direction);
        RHICmdList.SetShaderSampler(GetComputeShader(), Sampler.GetBaseIndex(), SamplerState);
        RHICmdList.SetShaderSampler(GetComputeShader(), AddSampler.GetBaseIndex(), AddSamplerState);
    }

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    }

    virtual bool Serialize(FArchive& Ar) override
    {
        bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
        Ar << InputTexture;
        Ar << InputAddTexture;
        Ar << RWOutputTexture;
        Ar << GaussBlurKernel1d;
        Ar << BlurLength;
        Ar << BlurDirection;
        Ar << Sampler;
        Ar << AddSampler;
        return bShaderHasOutdatedParameters;
    }

private:
    FShaderResourceParameter InputTexture;
    FShaderResourceParameter InputAddTexture;
    FRWShaderParameter RWOutputTexture;
    FShaderResourceParameter GaussBlurKernel1d;
    FShaderParameter BlurLength;
    FShaderParameter BlurDirection;
    FShaderResourceParameter Sampler;
    FShaderResourceParameter AddSampler;
};
IMPLEMENT_SHADER_TYPE(, FGaussianBlurAddComputeShader, TEXT("/Plugin/FisheyeCS4Camera/Private/GaussBlur1dAdd.usf"), TEXT("GaussBlur1dAddCS"), SF_Compute)

class FCombineComputeShader : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FCombineComputeShader, Global)

public:
    FCombineComputeShader() {}
    FCombineComputeShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
        : FGlobalShader(Initializer)
    {
        InputOriTexture.Bind(Initializer.ParameterMap, TEXT("InputOriTexture"));
        InputBlurTexture.Bind(Initializer.ParameterMap, TEXT("InputBlurTexture"));
        InputLUTTexture.Bind(Initializer.ParameterMap, TEXT("InputLUTTexture"));
        RWOutputTexture.Bind(Initializer.ParameterMap, TEXT("RWOutputTexture"));
        Sampler.Bind(Initializer.ParameterMap, TEXT("Sampler"));
        FisheyeMask.Bind(Initializer.ParameterMap, TEXT("FisheyeMask"));
    }

    void SetParameters(
        FRHICommandListImmediate& RHICmdList,
        FShaderResourceViewRHIRef& InputHighResOri,
        FShaderResourceViewRHIRef& InputLowResBlur,
        FShaderResourceViewRHIRef& InputLUT,
        FUnorderedAccessViewRHIRef& OutputUpscaled,
        FSamplerStateRHIRef& SamplerState,
        FShaderResourceViewRHIRef& FisheyeMaskSRV)
    {
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), InputOriTexture.GetBaseIndex(), InputHighResOri);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), InputBlurTexture.GetBaseIndex(), InputLowResBlur);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), InputLUTTexture.GetBaseIndex(), InputLUT);
        RHICmdList.SetUAVParameter(GetComputeShader(), RWOutputTexture.GetUAVIndex(), OutputUpscaled);
        RHICmdList.SetShaderSampler(GetComputeShader(), Sampler.GetBaseIndex(), SamplerState);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), FisheyeMask.GetBaseIndex(), FisheyeMaskSRV);
    }

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    }

    virtual bool Serialize(FArchive& Ar) override
    {
        bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
        Ar << InputOriTexture;
        Ar << InputBlurTexture;
        Ar << InputLUTTexture;
        Ar << RWOutputTexture;
        Ar << Sampler;
        Ar << FisheyeMask;
        return bShaderHasOutdatedParameters;
    }

private:
    FShaderResourceParameter InputOriTexture;
    FShaderResourceParameter InputBlurTexture;
    FShaderResourceParameter InputLUTTexture;
    FRWShaderParameter RWOutputTexture;
    FShaderResourceParameter Sampler;
    FShaderResourceParameter FisheyeMask;
};
IMPLEMENT_SHADER_TYPE(, FCombineComputeShader, TEXT("/Plugin/FisheyeCS4Camera/Private/CombineBloom.usf"), TEXT("CombineBloomCS"), SF_Compute)

class FLUTTextureComputeShader : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FLUTTextureComputeShader, Global)

public:
    FLUTTextureComputeShader() {}
    FLUTTextureComputeShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
        : FGlobalShader(Initializer)
    {
        LUTTexture.Bind(Initializer.ParameterMap, TEXT("LUTTexture"));
    }

    void SetParameters(
        FRHICommandListImmediate& RHICmdList,
        FUnorderedAccessViewRHIRef& LUTTextureRef)
    {
        RHICmdList.SetUAVParameter(GetComputeShader(), LUTTexture.GetUAVIndex(), LUTTextureRef);
    }

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    }

    virtual bool Serialize(FArchive& Ar) override
    {
        bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
        Ar << LUTTexture;
        return bShaderHasOutdatedParameters;
    }

private:
    FRWShaderParameter LUTTexture;
};
IMPLEMENT_SHADER_TYPE(, FLUTTextureComputeShader, TEXT("/Plugin/FisheyeCS4Camera/Private/LUT.usf"), TEXT("LUTCS"), SF_Compute)

FTexture2DRHIRef UFisheyeCS4CameraRendering::GetSharedLUT(FRHICommandListImmediate& RHICmdList) {
    static FTexture2DRHIRef Texture = CreateLUT(RHICmdList);
    return Texture;
}

FTexture2DRHIRef UFisheyeCS4CameraRendering::CreateLUT(FRHICommandListImmediate& RHICmdList)
{
    check(IsInRenderingThread());
    uint32 GroupSize = 32;
    uint32 SizeX = 1024;
    uint32 SizeY = 32;

    uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)SizeX, GroupSize);
    uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)SizeY, GroupSize);

    FRHIResourceCreateInfo OutputInfo;
    FTexture2DRHIRef OutputRHITexture = RHICreateTexture2D(SizeX, SizeY,
        PF_B8G8R8A8, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, OutputInfo);
    FUnorderedAccessViewRHIRef OutputUAV = RHICreateUnorderedAccessView(OutputRHITexture);
    TRefCountPtr<FRHITexture> OutputTextureRef(OutputRHITexture);

    TShaderMapRef<FLUTTextureComputeShader> LUTComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    RHICmdList.SetComputeShader(LUTComputeShader->GetComputeShader());

    LUTComputeShader->SetParameters(RHICmdList, OutputUAV);

    RHICmdList.TransitionResource(
        EResourceTransitionAccess::ERWNoBarrier,
        EResourceTransitionPipeline::EGfxToCompute,
        OutputUAV);

    DispatchComputeShader(RHICmdList, *LUTComputeShader, GroupSizeX, GroupSizeY, 1);
    return OutputRHITexture;
}

FTexture3DRHIRef UFisheyeCS4CameraRendering::CreateLUT3D(FRHICommandListImmediate& RHICmdList)
{
    check(IsInRenderingThread());
    uint32 GroupSize = 8;
    uint32 SizeX = 32;
    uint32 SizeY = 32;
    uint32 SizeZ = 32;

    uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)SizeX, GroupSize);
    uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)SizeY, GroupSize);
    uint32 GroupSizeZ = FMath::DivideAndRoundUp((uint32)SizeZ, GroupSize);

    FRHIResourceCreateInfo OutputInfo;
    FTexture3DRHIRef OutputRHITexture = RHICreateTexture3D(SizeX, SizeY, SizeZ,
        PF_B8G8R8A8, 1,  TexCreate_ShaderResource | TexCreate_UAV, OutputInfo);
    FUnorderedAccessViewRHIRef OutputUAV = RHICreateUnorderedAccessView(OutputRHITexture);
    TRefCountPtr<FRHITexture> OutputTextureRef(OutputRHITexture);

    TShaderMapRef<FLUTTextureComputeShader> LUTComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    RHICmdList.SetComputeShader(LUTComputeShader->GetComputeShader());

    LUTComputeShader->SetParameters(RHICmdList, OutputUAV);

    RHICmdList.TransitionResource(
        EResourceTransitionAccess::ERWNoBarrier,
        EResourceTransitionPipeline::EGfxToCompute,
        OutputUAV);

    DispatchComputeShader(RHICmdList, *LUTComputeShader, GroupSizeX, GroupSizeY, GroupSizeZ);
    return OutputRHITexture;
}


void UFisheyeCS4CameraRendering::UseComputeShaderArray_RenderThread(
    FRHICommandListImmediate& RHICmdList,
    TArray<FTextureRenderTargetResource*> InTextureRenderTargetResource,
    FTextureRenderTargetResource* OutTextureRenderTargetResource,
    FTextureRenderTargetResource* MipBloomTextureRenderTargetResource0,
    FIntPoint Resolution,
    int SampleNum)
{
    check(IsInRenderingThread());
    
    if (OutTextureRenderTargetResource && InTextureRenderTargetResource[0]->GetSizeX())
    {
        TArray<FTexture2DRHIRef> InRenderTargetTexture;
        for (int i = 0; i < InTextureRenderTargetResource.Num(); i++)
        {
            InRenderTargetTexture.Add(InTextureRenderTargetResource[i]->GetRenderTargetTexture());
        }

        FTexture2DRHIRef OutRenderTargetTexture = OutTextureRenderTargetResource->GetRenderTargetTexture();
        FTexture2DRHIRef MipBloomRenderTargetTexture = MipBloomTextureRenderTargetResource0->GetRenderTargetTexture();
        if (OutRenderTargetTexture.IsValid() && MipBloomRenderTargetTexture.IsValid())
        {
            uint32 GroupSize = 32;
            uint32 SizeX = OutTextureRenderTargetResource->GetSizeX();
            uint32 SizeY = OutTextureRenderTargetResource->GetSizeY();

            FIntPoint FullResolution = FIntPoint(SizeX, SizeY);
            uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)SizeX, GroupSize);
            uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)SizeY, GroupSize);

            FRHIResourceCreateInfo CreateInfo;
            FTexture2DRHIRef CreatedRHITexture = RHICreateTexture2D(SizeX, SizeY,
                PF_FloatRGBA, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, CreateInfo);
            FUnorderedAccessViewRHIRef TextureUAV = RHICreateUnorderedAccessView(CreatedRHITexture);
            TRefCountPtr<FRHITexture> OutputTextureRef(CreatedRHITexture);

            TArray<TRefCountPtr<FRHITexture>> InputTextureRef;
            for (int i = 0; i < InRenderTargetTexture.Num(); i++)
            {
                InputTextureRef.Add(TRefCountPtr<FRHITexture>(InRenderTargetTexture[i]));
            }
            if(SnitchNum == 4)
            {
                FTexture2DRHIRef DummyTexture = GBlackTexture->TextureRHI->GetTexture2D(); // 黑色占位纹理
                InputTextureRef.Add(TRefCountPtr<FRHITexture>(DummyTexture));
            }

            static uint32 Count = 0;
            static TShaderMapRef<FFisheyeCS4CameraComputeShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            RHICmdList.SetComputeShader(ComputeShader->GetComputeShader());

            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Bilinear>::GetRHI();
            ComputeShader->SetParameters(RHICmdList, InputTextureRef,
                OutputTextureRef, TextureUAV,SamplerState, 
                MapSamplePanelIDSRV[ID], SnitchNum);

            RHICmdList.TransitionResource(
                EResourceTransitionAccess::ERWNoBarrier,
                EResourceTransitionPipeline::EGfxToCompute,
                TextureUAV);
            DispatchComputeShader(RHICmdList, *ComputeShader, GroupSizeX, GroupSizeY, 1);

            RHICmdList.CopyTexture(CreatedRHITexture, OutRenderTargetTexture, FRHICopyTextureInfo());
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("UseComputeShader_RenderThread : not valid."));
        }
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("UseComputeShader_RenderThread : in and out is null."));
    }
}


void UFisheyeCS4CameraRendering::GenMipmap_RenderThread(
    FRHICommandListImmediate& RHICmdList,
    FTextureRenderTargetResource* InputTextureRenderTargetResource,
    FTextureRenderTargetResource* OutTextureRenderTargetResource)
{
    check(IsInRenderingThread());

    if (InputTextureRenderTargetResource)
    {
        FTexture2DRHIRef InputRenderTargetTexture = InputTextureRenderTargetResource->GetRenderTargetTexture();
        FTexture2DRHIRef OutRenderTargetTexture = OutTextureRenderTargetResource->GetRenderTargetTexture();
        if (InputRenderTargetTexture.IsValid() && OutRenderTargetTexture.IsValid())
        {
            FShaderResourceViewRHIRef MipmapInputSRV = RHICreateShaderResourceView(InputRenderTargetTexture, 0, 1, PF_FloatRGBA);

            uint32 GroupSize = 32;
            uint32 MipSizeX = FMath::DivideAndRoundUp(InputTextureRenderTargetResource->GetSizeX(), uint32(2));
            uint32 MipSizeY = FMath::DivideAndRoundUp(InputTextureRenderTargetResource->GetSizeY(), uint32(2));

            uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)MipSizeX, GroupSize);
            uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)MipSizeY, GroupSize);

            FRHIResourceCreateInfo MipmapOutputInfo;
            FTexture2DRHIRef MipmapOutputRHITexture = RHICreateTexture2D(MipSizeX, MipSizeY,
                PF_FloatRGBA, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, MipmapOutputInfo);
            FUnorderedAccessViewRHIRef MipmapOutputUAV = RHICreateUnorderedAccessView(MipmapOutputRHITexture);
            TRefCountPtr<FRHITexture> MipmapOutputTextureRef(MipmapOutputRHITexture);

            TShaderMapRef<FMipmapsComputeShader> MipmapComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            RHICmdList.SetComputeShader(MipmapComputeShader->GetComputeShader());

            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Bilinear>::GetRHI();
            MipmapComputeShader->SetParameters(RHICmdList, MipmapInputSRV, MipmapOutputUAV, SamplerState);

            RHICmdList.TransitionResource(
                EResourceTransitionAccess::ERWNoBarrier,
                EResourceTransitionPipeline::EGfxToCompute,
                MipmapOutputUAV);

            DispatchComputeShader(RHICmdList, *MipmapComputeShader, GroupSizeX, GroupSizeY, 1);
            RHICmdList.CopyTexture(MipmapOutputRHITexture, OutRenderTargetTexture, FRHICopyTextureInfo());
        }
    }
}

float UFisheyeCS4CameraRendering::GetBlurRadius(uint32 ViewSize, float KernelSizePercent)
{
    const float PercentToScale = 0.01f;

    const float DiameterToRadius = 0.5f;

    return static_cast<float>(ViewSize) * KernelSizePercent * PercentToScale * DiameterToRadius;
}

void UFisheyeCS4CameraRendering::GaussianBlur(
    FRHICommandListImmediate& RHICmdList, 
    FTextureRenderTargetResource* InTextureRenderTargetResource,
    FBloomStage& BloomStage, 
    int direction)
{
    check(IsInRenderingThread());
    bool bCalGaussKernel = false;
    TResourceArray<float>* GaussBlur1d = new TResourceArray<float>();
    float oldBloomStageSize = 0.0;

    if(InTextureRenderTargetResource)
    {
        FTexture2DRHIRef InputRenderTargetTexture = InTextureRenderTargetResource->GetRenderTargetTexture();
        if (InputRenderTargetTexture.IsValid())
        {
            FShaderResourceViewRHIRef GaussBlurInputSRV = RHICreateShaderResourceView(InputRenderTargetTexture, 0, 1, PF_FloatRGBA);

            uint32 GroupSize = 32;
            uint32 SizeX = InTextureRenderTargetResource->GetSizeX();
            uint32 SizeY = InTextureRenderTargetResource->GetSizeY();

            float BlurRadius = GetBlurRadius(SizeX, BloomStage.Size * 4.0);
            GaussBlur1d->Empty();
            Compute1DGaussianFilterKernel(*GaussBlur1d, 32, BlurRadius);

            if (direction)
            {
                for (int i = 0; i < (*GaussBlur1d).Num(); i++)
                {
                    (*GaussBlur1d)[i] *= (BloomStage.Tint.R);
                }
            }

            uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)SizeX, GroupSize);
            uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)SizeY, GroupSize);

            FRHIResourceCreateInfo OutputCreateInfo;
            FTexture2DRHIRef OutputRHITexture = RHICreateTexture2D(SizeX, SizeY,
                PF_FloatRGBA, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, OutputCreateInfo);
            FUnorderedAccessViewRHIRef TextureUAV = RHICreateUnorderedAccessView(OutputRHITexture);
            TRefCountPtr<FRHITexture> OutputTextureRef(OutputRHITexture);

            TShaderMapRef<FGaussianBlurComputeShader> GaussBlurComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            FStructuredBufferRHIRef GaussBlur1dBuffer;
            FShaderResourceViewRHIRef GaussBlur1dSRV;
            FRHIResourceCreateInfo GaussBlur1dCreateInfo;

            int BlurLength = GaussBlur1d->Num();
            GaussBlur1dCreateInfo.ResourceArray = GaussBlur1d;
            GaussBlur1dBuffer = RHICreateStructuredBuffer(sizeof(float), sizeof(float) * BlurLength,
                BUF_Static | BUF_ShaderResource, GaussBlur1dCreateInfo);
            GaussBlur1dSRV = RHICreateShaderResourceView(GaussBlur1dBuffer);

            RHICmdList.SetComputeShader(GaussBlurComputeShader->GetComputeShader());
            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Point>::GetRHI();

            GaussBlurComputeShader->SetParameters(RHICmdList, GaussBlurInputSRV,
                TextureUAV, GaussBlur1dSRV, BlurLength, direction, SamplerState);

            RHICmdList.TransitionResource(
                EResourceTransitionAccess::ERWNoBarrier,
                EResourceTransitionPipeline::EGfxToCompute,
                TextureUAV);
            DispatchComputeShader(RHICmdList, *GaussBlurComputeShader, GroupSizeX, GroupSizeY, 1);

            RHICmdList.CopyTexture(OutputRHITexture, InputRenderTargetTexture, FRHICopyTextureInfo());
        }
    }
}

void UFisheyeCS4CameraRendering::GaussianBlurAdd(
    FRHICommandListImmediate& RHICmdList,
    FTextureRenderTargetResource* InTextureRenderTargetResource,
    FTextureRenderTargetResource* InAddTextureRenderTargetResource,
    FBloomStage& BloomStage,
    int direction)
{
    check(IsInRenderingThread());
    static bool bCalGaussKernel = false;
    static TResourceArray<float>* GaussBlur1d = new TResourceArray<float>();
    static float oldBloomStageSize = 0.0;

    if (InTextureRenderTargetResource && InAddTextureRenderTargetResource)
    {
        FTexture2DRHIRef InputRenderTargetTexture = InTextureRenderTargetResource->GetRenderTargetTexture();
        FTexture2DRHIRef InputAddRenderTargetTexture = InAddTextureRenderTargetResource->GetRenderTargetTexture();
        if (InputRenderTargetTexture.IsValid() && InputAddRenderTargetTexture.IsValid())
        {
            FShaderResourceViewRHIRef GaussBlurInputSRV = RHICreateShaderResourceView(InputRenderTargetTexture, 0, 1, PF_FloatRGBA);
            FShaderResourceViewRHIRef GaussBlurInputAddSRV = RHICreateShaderResourceView(InputAddRenderTargetTexture, 0, 1, PF_FloatRGBA);

            uint32 GroupSize = 32;
            uint32 SizeX = InTextureRenderTargetResource->GetSizeX();
            uint32 SizeY = InTextureRenderTargetResource->GetSizeY();

            float BlurRadius = GetBlurRadius(SizeX, BloomStage.Size * 4.0);
            GaussBlur1d->Empty();
            Compute1DGaussianFilterKernel(*GaussBlur1d, 32, BlurRadius);

            if (direction)
            {
                for (int i = 0; i < (*GaussBlur1d).Num(); i++)
                {
                    (*GaussBlur1d)[i] *= (BloomStage.Tint.R);
                }
            }

            uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)SizeX, GroupSize);
            uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)SizeY, GroupSize);

            FRHIResourceCreateInfo OutputCreateInfo;
            FTexture2DRHIRef OutputRHITexture = RHICreateTexture2D(SizeX, SizeY,
                PF_FloatRGBA, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, OutputCreateInfo);
            FUnorderedAccessViewRHIRef TextureUAV = RHICreateUnorderedAccessView(OutputRHITexture);
            TRefCountPtr<FRHITexture> OutputTextureRef(OutputRHITexture);

            TShaderMapRef<FGaussianBlurAddComputeShader> GaussBlurAddComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            FStructuredBufferRHIRef GaussBlur1dBuffer;
            FShaderResourceViewRHIRef GaussBlur1dSRV;
            FRHIResourceCreateInfo GaussBlur1dCreateInfo;

            int BlurLength = GaussBlur1d->Num();
            GaussBlur1dCreateInfo.ResourceArray = GaussBlur1d;
            GaussBlur1dBuffer = RHICreateStructuredBuffer(sizeof(float), sizeof(float) * BlurLength,
                BUF_Static | BUF_ShaderResource, GaussBlur1dCreateInfo);
            GaussBlur1dSRV = RHICreateShaderResourceView(GaussBlur1dBuffer);

            RHICmdList.SetComputeShader(GaussBlurAddComputeShader->GetComputeShader());
            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Point>::GetRHI();
            FSamplerStateRHIRef AddSamplerState = TStaticSamplerState<SF_Bilinear>::GetRHI();

            GaussBlurAddComputeShader->SetParameters(RHICmdList, GaussBlurInputSRV, GaussBlurInputAddSRV,
                TextureUAV, GaussBlur1dSRV, BlurLength, direction, SamplerState, AddSamplerState);

            RHICmdList.TransitionResource(
                EResourceTransitionAccess::ERWNoBarrier,
                EResourceTransitionPipeline::EGfxToCompute,
                TextureUAV);
            DispatchComputeShader(RHICmdList, *GaussBlurAddComputeShader, GroupSizeX, GroupSizeY, 1);

            RHICmdList.CopyTexture(OutputRHITexture, InputRenderTargetTexture, FRHICopyTextureInfo());
        }

    }

}

void UFisheyeCS4CameraRendering::CombineBloom_RenderThread(
    FRHICommandListImmediate& RHICmdList,
    FTextureRenderTargetResource* InputOriTextureRenderTargetResource,
    FTextureRenderTargetResource* InputBlurTextureRenderTargetResource,
    FTextureRenderTargetResource* OutputTextureLDRRenderTargetResource,
    float cx,
    float cy)
{
    check(IsInRenderingThread());

    if (InputOriTextureRenderTargetResource && InputBlurTextureRenderTargetResource)
    {
        FTexture2DRHIRef InputOriRenderTargetTexture = InputOriTextureRenderTargetResource->GetRenderTargetTexture();
        FTexture2DRHIRef InputBlurRenderTargetTexture = InputBlurTextureRenderTargetResource->GetRenderTargetTexture();
        FTexture2DRHIRef InputLutTexture = GetSharedLUT(RHICmdList);
        FTexture2DRHIRef OutputLDRRenderTargetTexture = OutputTextureLDRRenderTargetResource->GetRenderTargetTexture();
        if (InputOriRenderTargetTexture.IsValid() && InputBlurRenderTargetTexture.IsValid())
        {
            FShaderResourceViewRHIRef InputOriSRV = RHICreateShaderResourceView(InputOriRenderTargetTexture, 0, 1, PF_FloatRGBA);
            FShaderResourceViewRHIRef InputBlurSRV = RHICreateShaderResourceView(InputBlurRenderTargetTexture, 0, 1, PF_FloatRGBA);
            FShaderResourceViewRHIRef InputLutSRV = RHICreateShaderResourceView(InputLutTexture, 0, 1, PF_B8G8R8A8);

            uint32 GroupSize = 32;
            uint32 SizeX = InputOriRenderTargetTexture->GetSizeX();
            uint32 SizeY = InputOriRenderTargetTexture->GetSizeY();
            if(!(SizeX * SizeY))
            {
                return;
            }

            uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)SizeX, GroupSize);
            uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)SizeY, GroupSize);

            FRHIResourceCreateInfo OutputInfo;
            FTexture2DRHIRef OutputRHITexture = RHICreateTexture2D(SizeX, SizeY,
                PF_B8G8R8A8, 1, 1, TexCreate_ShaderResource | TexCreate_UAV | TexCreate_SRGB, OutputInfo);
            FUnorderedAccessViewRHIRef OutputUAV = RHICreateUnorderedAccessView(OutputRHITexture);
            TRefCountPtr<FRHITexture> OutputTextureRef(OutputRHITexture);

            TShaderMapRef<FCombineComputeShader> CombineBloomComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            RHICmdList.SetComputeShader(CombineBloomComputeShader->GetComputeShader());

            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Bilinear>::GetRHI();

            CombineBloomComputeShader->SetParameters(
                RHICmdList, 
                InputOriSRV, 
                InputBlurSRV, 
                InputLutSRV,
                OutputUAV, 
                SamplerState,
                MapFisheyeMaskSRV[ID]);
            RHICmdList.TransitionResource(
                EResourceTransitionAccess::ERWNoBarrier,
                EResourceTransitionPipeline::EGfxToCompute,
                OutputUAV);

            DispatchComputeShader(RHICmdList, *CombineBloomComputeShader, GroupSizeX, GroupSizeY, 1);
            RHICmdList.CopyTexture(OutputRHITexture, OutputLDRRenderTargetTexture, FRHICopyTextureInfo());
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("Upscaling_RenderThread : InputHiResOriRenderTargetTexture.IsValid() && InputLowBlurRenderTargetTexture.IsValid() not valid."));
        }
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Upscaling_RenderThread : InputHiResOriTextureRenderTargetResource && InputLowBlurTextureRenderTargetResource not valid."));
    }
    
}


void UFisheyeCS4CameraRendering::CalGaussian1dKernel(TResourceArray<float>& Gaussian1dKernel, int BlurRadius, float sd)
{
    int n = BlurRadius * 2 + 1;
    float sum = 0.0;

    Gaussian1dKernel.Empty();
    Gaussian1dKernel.Init(1.0 / n, n);
    for(int i=0; i < n; i++)
    {
        int x = -BlurRadius + i;
        float dx = FMath::Abs(x);
        float ClampedOneMinusDX = FMath::Max(0.0f, 1.0f - dx);
        Gaussian1dKernel[i] = FMath::Exp(-16.7 * FMath::Square(dx / sd));
        sum += Gaussian1dKernel[i];
    }
    for (int i = 0; i < n; i++)
    {
        Gaussian1dKernel[i] /= sum;
    }
}

void UFisheyeCS4CameraRendering::UseComputeShaderArray(
    TArray<UTextureRenderTarget2D*> InputRenderTarget,
    UTextureRenderTarget2D* OutputRenderTarget,
    UTextureRenderTarget2D* OutputRenderTargetLDR,
    TArray<UTextureRenderTarget2D*> MipBloomRenderTarget,
    TArray<FBloomStage>& BloomStages,
    int SampleNum,
    float cx,
    float cy)
{
    check(IsInGameThread());
    FIntPoint Resolution;

    if (!OutputRenderTarget)
    {
        UE_LOG(LogTemp, Error, TEXT("no OutputRenderTarget"));
        return;
    }
    if (!OutputRenderTargetLDR)
    {
        UE_LOG(LogTemp, Error, TEXT("no OutputRenderTargetLDR"));
        return;
    }

    TArray<FTextureRenderTargetResource*> InputTextureRenderTargetResource;
    for (int i = 0; i < InputRenderTarget.Num(); i++)
    {
        InputTextureRenderTargetResource.Add(InputRenderTarget[i]->GameThread_GetRenderTargetResource());
    }
    FTextureRenderTargetResource* OutTextureRenderTargetResource = OutputRenderTarget->GameThread_GetRenderTargetResource();
    FTextureRenderTargetResource* OutTextureLDRRenderTargetResource = OutputRenderTargetLDR->GameThread_GetRenderTargetResource();
    Resolution.X = InputTextureRenderTargetResource[0]->GetSizeX();
    Resolution.Y = InputTextureRenderTargetResource[0]->GetSizeY();

    TArray<FTextureRenderTargetResource*> MipBloomTextureRenderTargetResource;
    for (int i = 0; i < MipBloomRenderTarget.Num(); i++)
    {
        MipBloomTextureRenderTargetResource.Add(MipBloomRenderTarget[i]->GameThread_GetRenderTargetResource());
    }


    if (OutTextureRenderTargetResource)
    {
        ENQUEUE_RENDER_COMMAND(FisheyeCS4Camera)
            (
                [&](FRHICommandListImmediate& RHICmdList)
        {
            UseComputeShaderArray_RenderThread
            (
                RHICmdList,
                InputTextureRenderTargetResource,
                OutTextureRenderTargetResource,
                MipBloomTextureRenderTargetResource[0],
                Resolution,
                SampleNum);
            GenMipmap_RenderThread(
                RHICmdList,
                OutTextureRenderTargetResource,
                MipBloomTextureRenderTargetResource[0]);
            for(int i = 0; i < MipBloomRenderTarget.Num() - 1; i++)
            {
                GenMipmap_RenderThread(
                    RHICmdList,
                    MipBloomTextureRenderTargetResource[i],
                    MipBloomTextureRenderTargetResource[i + 1]);
            }

            float TintScale = 1.0f / 6.0f;

                GaussianBlur(
                    RHICmdList,
                    MipBloomTextureRenderTargetResource.Last(),
                    BloomStages[0],
                    0);
                GaussianBlur(
                    RHICmdList,
                    MipBloomTextureRenderTargetResource.Last(),
                    BloomStages[0],
                    1);
                for(int i = MipBloomRenderTarget.Num() - 2 ; i >= 0; i--)
                {
                    GaussianBlur(
                        RHICmdList,
                        MipBloomTextureRenderTargetResource[i],
                        BloomStages[MipBloomRenderTarget.Num() - i - 1],
                        0);
                    GaussianBlurAdd(
                        RHICmdList,
                        MipBloomTextureRenderTargetResource[i],
                        MipBloomTextureRenderTargetResource[i+1],
                        BloomStages[MipBloomRenderTarget.Num() - i - 1],
                        1);
                }
            CombineBloom_RenderThread(
                RHICmdList,
                OutTextureRenderTargetResource,
                MipBloomTextureRenderTargetResource[0],
                OutTextureLDRRenderTargetResource,
                cx,
                cy);
        }
        );
        FlushRenderingCommands();
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("no ENQUEUE_RENDER_COMMAND"));
    }
}


bool UFisheyeCS4CameraRendering::SaveResourceArrayToFile(const FString& FilePath, const TResourceArray<int32>& Data)
{
    TUniquePtr<FArchive> FileWriter(IFileManager::Get().CreateFileWriter(*FilePath));
    if (!FileWriter)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to open file for writing: %s"), *FilePath);
        return false;
    }

    int32 Num = Data.Num();
    *FileWriter << Num; 

    for (int32 i = 0; i < Num; ++i)
    {
        int32 Value = Data[i];
        *FileWriter << Value;
    }

    FileWriter->Close();
    return true;
}

bool UFisheyeCS4CameraRendering::SaveResourceArrayToFile(const FString& FilePath, const TResourceArray<float>& Data)
{
    TUniquePtr<FArchive> FileWriter(IFileManager::Get().CreateFileWriter(*FilePath));
    if (!FileWriter)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to open file for writing: %s"), *FilePath);
        return false;
    }

    int32 Num = Data.Num();
    *FileWriter << Num;

    for (int32 i = 0; i < Num; ++i)
    {
        float Value = Data[i];
        *FileWriter << Value;
    }

    FileWriter->Close();
    return true;
}

bool UFisheyeCS4CameraRendering::LoadResourceArrayFromFile(const FString& FilePath, TResourceArray<int32>& OutData)
{
    TUniquePtr<FArchive> FileReader(IFileManager::Get().CreateFileReader(*FilePath));
    if (!FileReader)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to open file for reading: %s"), *FilePath);
        return false;
    }

    int32 Num = 0;
    *FileReader << Num;
    check(Num == OutData.Num())

    for (int32 i = 0; i < Num; ++i)
    {
        int32 Value = 0;
        *FileReader << Value;
        OutData[i] = Value;
    }

    FileReader->Close();
    return true;
}


bool UFisheyeCS4CameraRendering::LoadResourceArrayFromFile(const FString& FilePath, TResourceArray<float>& OutData)
{
    TUniquePtr<FArchive> FileReader(IFileManager::Get().CreateFileReader(*FilePath));
    if (!FileReader)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to open file for reading: %s"), *FilePath);
        return false;
    }

    int32 Num = 0;
    *FileReader << Num;
    check(Num == OutData.Num())

    for (int32 i = 0; i < Num; ++i)
    {
        float Value = 0;
        *FileReader << Value;
        OutData[i] = Value;
    }

    FileReader->Close();
    return true;
}

int32 UFisheyeCS4CameraRendering::FindBinFilesInSavedDir(const FString& MatchString, TArray<FString>& OutFoundFiles)
{
    FString SearchDir = FPaths::ProjectSavedDir();

    IFileManager& FileManager = IFileManager::Get();
    TArray<FString> AllFiles;
    FileManager.FindFilesRecursive(AllFiles, *SearchDir, TEXT("*.bin"), true, false);

    for (const FString& FilePath : AllFiles)
    {
        FString FileName = FPaths::GetCleanFilename(FilePath);
        if (FileName.Contains(MatchString))
        {
            OutFoundFiles.Add(FilePath);
        }
    }

    return OutFoundFiles.Num();
}

int UFisheyeCS4CameraRendering::HasCommonFace(FPointInfo& P1, FPointInfo& P2)
{
    for (int i = 0; i < P1.FaceIndex.Num(); i++)
    {
        for (int j = 0; j < P2.FaceIndex.Num(); j++)
        {
            if (P1.FaceIndex[i] == P2.FaceIndex[j])
            {
                return P1.FaceIndex[i];
            }
        }
    }
    return -1;
}

FPlane UFisheyeCS4CameraRendering::NormalizePlane(const FPlane& P)
{
    FVector N = FVector(P.X, P.Y, P.Z);
    float Len = N.Size();
    if (Len <= EPS)
    {
        return FPlane(0, 0, 0, 0);
    }
    FVector Normalized = N / Len;
    return FPlane(Normalized, P.W / Len);
}

FVector UFisheyeCS4CameraRendering::IntersectThreePlanes(const FPlane& P1, const FPlane& P2, const FPlane& P3)
{

    FPlane NP1 = NormalizePlane(P1);
    FPlane NP2 = NormalizePlane(P2);
    FPlane NP3 = NormalizePlane(P3);

    FVector a = NP1.GetUnsafeNormal();
    FVector b = NP2.GetUnsafeNormal();
    FVector c = NP3.GetUnsafeNormal();
    a.Normalize();
    b.Normalize();
    c.Normalize();

    float d1 = -NP1.W;
    float d2 = -NP2.W;
    float d3 = -NP3.W;

    FVector bxc = FVector::CrossProduct(b, c);
    FVector cxa = FVector::CrossProduct(c, a);
    FVector axb = FVector::CrossProduct(a, b);

    const float denom = FVector::DotProduct(a, bxc);
    if (FMath::IsNearlyZero(denom))
    {
        return FVector::ZeroVector;
    }

    const FVector result = (-d1 * bxc + -d2 * cxa + -d3 * axb) / denom;

    if (result.ContainsNaN() ||
        !FMath::IsFinite(result.X) ||
        !FMath::IsFinite(result.Y) ||
        !FMath::IsFinite(result.Z))
    {
        return FVector::ZeroVector;
    }
    return result;
}

float UFisheyeCS4CameraRendering::GetSegmentTProjection(const FVector& A, const FVector& B, const FVector& P)
{
    FVector AB = B - A;
    FVector AP = P - A;

    float LengthSq = AB.SizeSquared();
    if (LengthSq < EPS)
        return 0.0f;

    float T = FVector::DotProduct(AB, AP) / LengthSq;
    return T;
}

bool UFisheyeCS4CameraRendering::IsPointOnPlane(const FVector& Point, const FPlane& Plane, float Tolerance = EPS)
{
    return FMath::Abs(Plane.PlaneDot(Point)) <= Tolerance;
}

bool UFisheyeCS4CameraRendering::IsPointOnEdge(FVector Point, float Tolerance = EPS)
{
    return (FMath::IsNearlyZero(Point.X, EPS) ||
        FMath::IsNearlyZero(Point.Y, EPS) ||
        FMath::IsNearlyEqual(Point.X, 1.0f * float(Width), EPS) ||
        FMath::IsNearlyEqual(Point.Y, 1.0f * float(Width), EPS));
}

bool UFisheyeCS4CameraRendering::IsOnSameEdge(FVector P1, FVector P2, float Tolerance = EPS)
{
    return ((FMath::IsNearlyZero(P1.X, EPS) && FMath::IsNearlyZero(P2.X, EPS)) ||
        (FMath::IsNearlyZero(P1.Y, EPS) && FMath::IsNearlyZero(P2.Y, EPS)) ||
        (FMath::IsNearlyEqual(P1.X, 1.0f * float(Width), EPS) &&
            FMath::IsNearlyEqual(P2.X, 1.0f * float(Width), EPS)) ||
        (FMath::IsNearlyEqual(P1.Y, 1.0f * float(Width), EPS) &&
            FMath::IsNearlyEqual(P2.Y, 1.0f * float(Width), EPS)));
}

bool UFisheyeCS4CameraRendering::AddIfCantFind(TArray<FVector>& Group, FVector Point)
{
    bool Found = false;
    for (FVector P : Group)
    {
        if (P.Equals(Point, EPS))
        {
            Found = true;
            break;
        }
    }
    if (Found)
    {
        return false;
    }
    Group.Add(Point);
    return true;
}

double UFisheyeCS4CameraRendering::Halton(int32 Index, int32 Base)
{
    double Result = 0.0;
    double F = 1.0 / Base;
    int32 i = Index;
    while (i > 0)
    {
        Result += F * (i % Base);
        i /= Base;
        F /= Base;
    }
    return Result;
}

// Generate the first 15 two-dimensional Halton sampling points, using base 2 and base 3, and return an array of FVector2D
TArray<FVector2D> UFisheyeCS4CameraRendering::GenerateHalton2DPoints(int32 NumPoints)
{
    TArray<FVector2D> Points;
    Points.Reserve(NumPoints);

    for (int32 i = 1; i <= NumPoints; ++i)
    {
        double X = Halton(i, 2);
        double Y = Halton(i, 3);
        Points.Add(FVector2D(X, Y));
    }

    return Points;
}

bool AreIndicesAdjacent(int a, int b, int N)
{
    return ((b == (a + 1) % N) || (a == (b + 1) % N));
}

void UFisheyeCS4CameraRendering::SplitPoints(int pi, int pj, TArray<FPointInfo>& InputPoints, TArray<TArray<FVector>>& OutGroups)
{
    int CountBefore = InputPoints.Num();
    bool bAddNewPoint = false;

    int baddpoint = 0;
    // Generate split points on the edges between faces
    for (int i = 0; i < CountBefore; i++)
    {
        FSegment seg(InputPoints[i], InputPoints[(i + 1) % CountBefore]);
        int SharedFaceCount = 0;

        for (int j : seg.PStart.FaceIndex)
        {
            for (int k : seg.PEnd.FaceIndex)
            {
                if (j == k)
                {
                    SharedFaceCount++;
                }
            }
        }

        // If two points are not coplanar, boundary points need to be calculated
        if (SharedFaceCount == 0)
        {
            FPlane splitPlane(seg.PStart.WorldPos, seg.PEnd.WorldPos, FVector::ZeroVector);
            TArray<FPointInfo> InsertedPoints;
            TArray<float> TList;

            for (int FaceA = 0; FaceA < PlaneArray.Num() - 1; FaceA++)
            {
                for (int FaceB = FaceA + 1; FaceB < PlaneArray.Num(); FaceB++)
                {
                    // Exclude invalid indices (i.e., only allow combinations of faces from A and B respectively)
                    if (!(seg.PStart.FaceIndex.Contains(FaceA) || seg.PEnd.FaceIndex.Contains(FaceA) ||
                        seg.PStart.FaceIndex.Contains(FaceB) || seg.PEnd.FaceIndex.Contains(FaceB)))
                        continue;

                    const FPlane& P1 = splitPlane;
                    const FPlane& P2 = PlaneArray[FaceA];
                    const FPlane& P3 = PlaneArray[FaceB];

                    FVector pt = IntersectThreePlanes(P1, P2, P3);
                    if (pt.Equals(FVector::ZeroVector))
                        continue;

                    bool bAlreadyInserted = false;
                    for (const FPointInfo& Existing : InsertedPoints)
                    {
                        if (FVector::DistSquared(Existing.WorldPos, pt) < EPS)
                        {
                            bAlreadyInserted = true;
                            break;
                        }
                    }
                    if (bAlreadyInserted || !IsInRange(pt))
                        continue;

                    float t = GetSegmentTProjection(seg.PStart.WorldPos, seg.PEnd.WorldPos, pt);

                    if (t < EPS || t > 1.0f - EPS)
                        continue;

                    //if (FMath::IsNearlyEqual(t, 0.0f, EPS)) t = 0.0f;
                    //else if (FMath::IsNearlyEqual(t, 1.0f, EPS)) t = 1.0f;

                    // Extract which faces they fall on
                    TArray<int> FaceIDs;
                    for (int n = 0; n < PlaneArray.Num(); ++n)
                    {
                        if (IsPointOnPlane(pt, PlaneArray[n]))
                        {
                            FaceIDs.Add(n);
                        }
                    }

                    FPointInfo NewPt(pt, FaceIDs);
                    bAddNewPoint = true;

                    // Insertion sort in ascending order by t value
                    bool bInserted = false;
                    for (int p = 0; p < TList.Num(); ++p)
                    {
                        if (t < TList[p])
                        {
                            InsertedPoints.Insert(NewPt, p);
                            TList.Insert(t, p);
                            bInserted = true;
                            break;
                        }
                    }
                    if (!bInserted)
                    {
                        InsertedPoints.Add(NewPt);
                        TList.Add(t);
                    }
                }
            }


            // Insert intersection points into InputPoints (insert from back to front to prevent index confusion)
            for (int q = InsertedPoints.Num() - 1; q >= 0; q--)
            {
                InputPoints.Insert(InsertedPoints[q], i + 1);
            }

            i += InsertedPoints.Num(); // 跳过新插入的交点
            CountBefore += InsertedPoints.Num();
        }
    }
    //Distribute points in 2D coordinates. The output here is the 2D coordinates on each face, with FVector.z = 0.
    {
        for (FPointInfo Point : InputPoints)
        {
            for (int faceidx : Point.FaceIndex)
            {
                OutGroups[faceidx].Add(LocalSpace2Panel(faceidx, Point.WorldPos));
            }
        }
        //How to determine whether to add a three-face vertex? It depends on whether the starting and ending points lie on the same edge.
        if (bAddNewPoint)
        {
            FVector LeftTop = FVector(0.0f, 0.0f, 0.0f);
            FVector RightTop = FVector(1.0f, 0.0f, 0.0f) * float(Width);
            FVector LeftBottom = FVector(0.0f, 1.0f, 0.0f) * float(Width);
            FVector RightBottom = FVector(1.0f, 1.0f, 0.0f) * float(Width);
            //First, find the starting and ending points, i.e., the entry point and exit point.
            for (int i = 0; i < OutGroups.Num(); i++)
            {
                TArray<FVector>& Group = OutGroups[i];
                if (Group.Num() > 1)
                {
                    FVector InFace = FVector(-1.0f, -1.0f, -1.0f);
                    FVector OutFace = FVector(-1.0f, -1.0f, -1.0f);
                    int InFaceIdx = -1;
                    int OutFaceIdx = -1;
                    for (int j = 0; j < Group.Num(); j++)
                    {
                        FVector Start = Group[j];
                        FVector End = Group[(j + 1) % Group.Num()];
                        bool IsStartOnEdge = IsPointOnEdge(Start);
                        bool IsEndOnEdge = IsPointOnEdge(End);
                        if (IsStartOnEdge && !IsEndOnEdge)
                        {
                            InFace = Start;
                            InFaceIdx = j;
                        }
                        else if (!IsStartOnEdge && IsEndOnEdge)
                        {
                            OutFace = End;
                            OutFaceIdx = (j + 1) % Group.Num();
                        }
                        else if (IsStartOnEdge && IsEndOnEdge && !IsOnSameEdge(Start, End) && Group.Num() >= 2)
                        {
                            InFace = Start;
                            OutFace = End;
                            InFaceIdx = j;
                            OutFaceIdx = (j + 1) % Group.Num();
                        }
                    }

                    if (((FMath::IsNearlyZero(InFace.X, EPS) && 
                        FMath::IsNearlyZero(OutFace.Y, EPS) && 
                        !InFace.Equals(LeftTop, EPS) && 
                        !OutFace.Equals(LeftTop, EPS)) ||
                        (FMath::IsNearlyZero(InFace.Y, EPS) && 
                            FMath::IsNearlyZero(OutFace.X, EPS) && 
                            !InFace.Equals(LeftTop, EPS) && 
                            !OutFace.Equals(LeftTop, EPS))) && (AreIndicesAdjacent(InFaceIdx,OutFaceIdx, Group.Num())))
                    {
                        AddIfCantFind(Group, LeftTop);
                    }
                    else if (((FMath::IsNearlyEqual(InFace.X, 1.0f * float(Width), EPS) &&
                        FMath::IsNearlyZero(OutFace.Y, EPS) && 
                        !InFace.Equals(RightTop, EPS) && 
                        !OutFace.Equals(RightTop, EPS)) ||
                        (FMath::IsNearlyZero(InFace.Y, EPS) && 
                            FMath::IsNearlyEqual(OutFace.X, 1.0f * float(Width), EPS) &&
                            !InFace.Equals(RightTop, EPS) && 
                            !OutFace.Equals(RightTop, EPS))) && (AreIndicesAdjacent(InFaceIdx,OutFaceIdx, Group.Num())))
                    {
                        AddIfCantFind(Group, RightTop);
                    }
                    else if (((FMath::IsNearlyZero(InFace.X, EPS) && 
                        FMath::IsNearlyEqual(OutFace.Y, 1.0f * float(Width), EPS) &&
                        !InFace.Equals(LeftBottom, EPS) && 
                        !OutFace.Equals(LeftBottom, EPS)) ||
                        (FMath::IsNearlyEqual(InFace.Y, 1.0f * float(Width), EPS) &&
                            FMath::IsNearlyZero(OutFace.X, EPS) && 
                            !InFace.Equals(LeftBottom, EPS) && 
                            !OutFace.Equals(LeftBottom, EPS))) && (AreIndicesAdjacent(InFaceIdx,OutFaceIdx, Group.Num())))
                    {
                        AddIfCantFind(Group, LeftBottom);
                    }
                    else if (((FMath::IsNearlyEqual(InFace.Y, 1.0f * float(Width), EPS) &&
                        FMath::IsNearlyEqual(OutFace.X, 1.0f * float(Width), EPS) &&
                        !InFace.Equals(RightBottom, EPS) && 
                        !OutFace.Equals(RightBottom, EPS)) ||
                        (FMath::IsNearlyEqual(InFace.X, 1.0f * float(Width), EPS) &&
                            FMath::IsNearlyEqual(OutFace.Y, 1.0f * float(Width), EPS) &&
                            !InFace.Equals(RightBottom, EPS) && 
                            !OutFace.Equals(RightBottom, EPS))) && (AreIndicesAdjacent(InFaceIdx,OutFaceIdx, Group.Num())))
                    {
                        AddIfCantFind(Group, RightBottom);
                    }
                    else if ((((FMath::IsNearlyZero(InFace.Y, EPS) && 
                        FMath::IsNearlyEqual(OutFace.Y, 1.0f, EPS) && 
                        !InFace.Equals(RightTop, EPS) && 
                        !OutFace.Equals(RightBottom, EPS)) ||
                        (FMath::IsNearlyEqual(InFace.Y, 1.0f, EPS) && 
                            FMath::IsNearlyZero(OutFace.Y, EPS) && 
                            !InFace.Equals(RightBottom, EPS) && 
                            !OutFace.Equals(RightTop, EPS))) && (AreIndicesAdjacent(InFaceIdx,OutFaceIdx, Group.Num()))))
                    {
                        if (i == 0)
                        {
                            AddIfCantFind(Group, RightTop);
                            AddIfCantFind(Group, RightBottom);
                        }
                        if (i == 1)
                        {
                            AddIfCantFind(Group, LeftTop);
                            AddIfCantFind(Group, LeftBottom);
                        }
                    }
                }
                else
                {
                    Group.Empty();
                }

            }
        }
        for (TArray<FVector> Group : OutGroups)
        {
            float s = ComputePolygonArea2D(Group);
            if (s < EPS)
                Group.Empty();
        }

    }

}

// Determine whether two line segments intersect in the 2D plane
bool UFisheyeCS4CameraRendering::DoSegmentsIntersect(const FVector& p1, const FVector& p2, const FVector& q1, const FVector& q2)
{
    auto Cross = [](const FVector2D& a, const FVector2D& b) {
        return a.X * b.Y - a.Y * b.X;
    };

    auto To2D = [](const FVector& v) {
        return FVector2D(v.X, v.Y);
    };

    FVector2D r = To2D(p2 - p1);
    FVector2D s = To2D(q2 - q1);
    FVector2D pq = To2D(q1 - p1);

    float rxs = Cross(r, s);
    float pqxr = Cross(pq, r);

    // Parallel or collinear
    if (FMath::IsNearlyZero(rxs)) return false; 

    float t = Cross(pq, s) / rxs;
    float u = pqxr / rxs;

    return (t > 0 && t < 1) && (u > 0 && u < 1);
}

// Determine whether the polygon is a simple polygon (i.e., non-self-intersecting)
bool UFisheyeCS4CameraRendering::IsSimplePolygon(const TArray<FVector>& Points)
{
    int32 Num = Points.Num();
    for (int32 i = 0; i < Num; ++i)
    {
        FVector A1 = Points[i];
        FVector A2 = Points[(i + 1) % Num];

        for (int32 j = i + 1; j < Num; ++j)
        {
            // Skip shared vertices or adjacent edges
            if ((j == i) || (j == (i + 1) % Num) || ((i == 0 && j == Num - 1))) continue;

            FVector B1 = Points[j];
            FVector B2 = Points[(j + 1) % Num];

            if (DoSegmentsIntersect(A1, A2, B1, B2))
            {
                return false;
            }
        }
    }
    return true;
}

// Calculate the area of a 2D polygon, automatically detect if it self-intersects.
float UFisheyeCS4CameraRendering::ComputePolygonArea2D(const TArray<FVector>& Points)
{
    int32 NumPoints = Points.Num();
    if (NumPoints < 3) return 0.0f;

    if (!IsSimplePolygon(Points))
    {
        return -1.0f;
    }

    // For translated local coordinates
    TArray<FVector2D> PLocal;
    PLocal.Reserve(NumPoints);

    FVector2D Origin(Points[0].X, Points[0].Y);
    for (const auto& P : Points)
    {
        PLocal.Add(FVector2D(P.X - Origin.X, P.Y - Origin.Y));
    }

    // Shoelace formula
    float Area = 0.0f;
    for (int32 i = 0; i < NumPoints; ++i)
    {
        const FVector2D& P1 = PLocal[i];
        const FVector2D& P2 = PLocal[(i + 1) % NumPoints];
        Area += (P1.X * P2.Y - P2.X * P1.Y);
    }

    return FMath::Abs(Area) * 0.5f;
}


void UFisheyeCS4CameraRendering::CheckPointsFaces(TArray<FPointInfo>& InputPoints)
{
    for(int i = 0; i < InputPoints.Num(); i++)
    {
        for(int j = 0; j < PlaneArray.Num(); j++)
        {
            if (IsPointOnPlane(InputPoints[i].WorldPos, PlaneArray[j]))
            {
                InputPoints[i].FaceIndex.Add(j);
            }
        }
    }
}

bool SolveThetaBisection(float r, float d1, float d2, float d3, float d4,
    float theta_min, float theta_max, float& theta_out)
{
    auto f = [&](float theta) {
        float th2 = theta * theta;
        float th4 = th2 * th2;
        float th6 = th4 * th2;
        float th8 = th4 * th4;
        return theta * (1.0f + d1 * th2 + d2 * th4 + d3 * th6 + d4 * th8) - r;
    };

    float a = theta_min;
    float b = theta_max;
    float fa = f(a);
    float fb = f(b);

    if (fa * fb > 0.0f) {
        return false; 
    }

    for (int iter = 0; iter < 50; iter++) {
        float c = 0.5f * (a + b);
        float fc = f(c);

        if (fabs(fc) < 1e-6f || fabs(b - a) < 1e-6f) {
            theta_out = c;
            return true;
        }

        if (fa * fc < 0.0f) {
            b = c; fb = fc;
        }
        else {
            a = c; fa = fc;
        }
    }

    theta_out = 0.5f * (a + b);
    return true; // 返回近似解
}


bool SolveThetaNewton(float r, float d1, float d2, float d3, float d4,
    float theta0, float theta_min, float theta_max, float& theta_out)
{
    auto f_and_fprime = [&](float theta, float &fval, float &fprime) {
        float th2 = theta * theta;
        float th4 = th2 * th2;
        float th6 = th4 * th2;
        float th8 = th4 * th4;

        // f(theta)
        fval = theta * (1.0f + d1 * th2 + d2 * th4 + d3 * th6 + d4 * th8) - r;

        // f'(theta)
        fprime = 1.0f + 3.0f * d1 * th2 + 5.0f * d2 * th4 + 7.0f * d3 * th6 + 9.0f * d4 * th8;
    };

    // First, check whether there may be a root within the interval.
    auto f = [&](float theta) {
        float th2 = theta * theta;
        float th4 = th2 * th2;
        float th6 = th4 * th2;
        float th8 = th4 * th4;
        return theta * (1.0f + d1 * th2 + d2 * th4 + d3 * th6 + d4 * th8) - r;
    };

    // Same sign at both ends of the interval → no solution
    float fa = f(theta_min);
    float fb = f(theta_max);
    if (fa * fb > 0.0f) {
        return false;
    }

    float theta = FMath::Clamp(theta0, theta_min, theta_max);

    for (int iter = 0; iter < 50; iter++) {
        float fval, fprime;
        f_and_fprime(theta, fval, fprime);

        // Convergence successful
        if (fabs(fval) < 1e-6f) {
            theta_out = theta;
            return true; 
        }

        // The derivative is close to 0, unable to update.
        if (fabs(fprime) < 1e-12f) {
            return false; 
        }

        float next = theta - fval / fprime;

        // If out of bounds, fall back to the midpoint of the interval (to ensure numerical stability)
        if (next < theta_min || next > theta_max) {
            next = 0.5f * (theta_min + theta_max);
        }

        theta = next;
    }

    return false; 
}

void UFisheyeCS4CameraRendering::CalPixelsRelationship(
    FIntPoint Resolution,
    int TextureNum,
    float FOV,
    float d1,
    float d2,
    float d3,
    float d4,
    float fx,
    float fy,
    float cx,
    float cy)
{
    SnitchNum = TextureNum;
    FString LongID = TEXT("snitchnum_") + FString::FromInt(SnitchNum) +
        TEXT("_x_") + FString::FromInt(Resolution.X) + TEXT("_y_") + FString::FromInt(Resolution.Y) +
        TEXT("_") + TEXT("FOV") + TEXT("_") + FString::Printf(TEXT("%.6f"), FOV) +
        TEXT("_") + TEXT("d1") + TEXT("_") + FString::Printf(TEXT("%.6f"), d1) +
        TEXT("_") + TEXT("d2") + TEXT("_") + FString::Printf(TEXT("%.6f"), d2) +
        TEXT("_") + TEXT("d3") + TEXT("_") + FString::Printf(TEXT("%.6f"), d3) +
        TEXT("_") + TEXT("d4") + TEXT("_") + FString::Printf(TEXT("%.6f"), d4) +
        TEXT("_") + TEXT("fx") + TEXT("_") + FString::Printf(TEXT("%.6f"), fx) +
        TEXT("_") + TEXT("fy") + TEXT("_") + FString::Printf(TEXT("%.6f"), fy) +
        TEXT("_") + TEXT("cx") + TEXT("_") + FString::Printf(TEXT("%.6f"), cx) +
        TEXT("_") + TEXT("cy") + TEXT("_") + FString::Printf(TEXT("%.6f"), cy);
    ID = EncodeIDToFileName(LongID);
    Width = FMath::Min(Resolution.X, Resolution.Y);
    Radius = float(Width) / 2;
    UE_LOG(LogTemp, Warning, TEXT("LongID is %s"), *LongID);
    UE_LOG(LogTemp, Warning, TEXT("ID is %s"), *ID);
    // If the ID table and Mask table are in memory, return directly
    if(MapSamplePanelID.Contains(ID) && MapFisheyeMask.Contains(ID))
    {
        UE_LOG(LogTemp, Warning, TEXT("LUT found in RAM! MapSamplePanelID.Contains(ID) && MapFisheyeMask.Contains(ID)"));
        return;
    }
    // If tables are not available, read from disk or compute.
    else    
    {
        TArray<FString> OutFoundIDFiles;
        TArray<FString> OutFoundMaskFiles;

        TSharedPtr<TResourceArray<int>> SamplePanelIDptr = MakeShared<TResourceArray<int>>();
        SamplePanelIDptr->Init(-1, Resolution.X * Resolution.Y * TopNPixel);
        MapSamplePanelID.Add(ID, SamplePanelIDptr);


        TSharedPtr<TResourceArray<float>> FisheyeMaskptr = MakeShared<TResourceArray<float>>();
        FisheyeMaskptr->Init(0.0f, Resolution.X * Resolution.Y);
        MapFisheyeMask.Add(ID, FisheyeMaskptr);

        // If stored, read directly and return.
        if(FindBinFilesInSavedDir(TEXT("ID_") + ID + TEXT(".bin"), OutFoundIDFiles) &&
            FindBinFilesInSavedDir(TEXT("Mask_") + ID + TEXT(".bin"), OutFoundMaskFiles))
        {
            UE_LOG(LogTemp, Warning, TEXT("LUT found in Disk!"));
            if (LoadResourceArrayFromFile(OutFoundIDFiles[0], *SamplePanelIDptr))
            {
                UE_LOG(LogTemp, Log, TEXT("Successfully loaded data from: %s"), *OutFoundIDFiles[0]);

            }
            else
            {
                UE_LOG(LogTemp, Error, TEXT("Failed to load data from: %s"), *OutFoundIDFiles[0]);
            }
            if (LoadResourceArrayFromFile(OutFoundMaskFiles[0], *FisheyeMaskptr))
            {
                UE_LOG(LogTemp, Log, TEXT("Successfully loaded data from: %s"), *OutFoundMaskFiles[0]);

            }
            else
            {
                UE_LOG(LogTemp, Error, TEXT("Failed to load data from: %s"), *OutFoundMaskFiles[0]);
            }
        }
        // If neither tables nor storage exist in memory, compute.
        else
        {
            if (SmallerAndEqual(FOV, 180.0f, EPS) && SnitchNum == 4)
            {
                PlaneArray.Add(FPlane(1.0f, -1.0f, 0.0f, UE_SQRT_2 * Radius));
                PlaneArray.Add(FPlane(1.0f, 1.0f, 0.0f, UE_SQRT_2 * Radius));
                PlaneArray.Add(FPlane(0.0f, 0.0f, 1.0f, 1.0f * Radius));
                PlaneArray.Add(FPlane(0.0f, 0.0f, 1.0f, -1.0f * Radius));
            }
            else if (SmallerAndEqual(FOV, 270.0f, EPS) || SnitchNum == 5)
            {
                PlaneArray.Add(FPlane(1.0f, 0.0f, 0.0f, 1.0f * Radius));
                PlaneArray.Add(FPlane(0.0f, 1.0f, 0.0f, -1.0f * Radius));
                PlaneArray.Add(FPlane(0.0f, 1.0f, 0.0f, 1.0f * Radius));
                PlaneArray.Add(FPlane(0.0f, 0.0f, 1.0f, 1.0f * Radius));
                PlaneArray.Add(FPlane(0.0f, 0.0f, 1.0f, -1.0f * Radius));
            }else
            {
                UE_LOG(LogTemp, Error, TEXT("FOV %.2f is too large! Max supported is 270."), FOV);
                checkf(false, TEXT("FOV %.2f is too large! Supported range is <= 270."), FOV);
            }

            UE_LOG(LogTemp, Warning, TEXT("LUT can't found in RAM and Disk! Cal"));
            float Size = 1.0f;
            float Step = Size / float(n);
            const FVector2D Center(cx, cy);
            const float theta_f = FMath::DegreesToRadians(FOV * 0.5f);

            TArray<FVector2D> HaltonPoints;
            HaltonPoints = this->GenerateHalton2DPoints(15);

            int TotalPixels = Resolution.X * Resolution.Y;
            ParallelFor(TotalPixels, [&](int32 ii)
            {
                int i = ii % Resolution.X;
                int j = ii / Resolution.X;
                FVector2D Start = FVector2D(float(i), float(j));
                TArray<FPointInfo> Input;
                TArray<TArray<FVector>> OutGroups;
                for (int pidx = 0; pidx < 4 * n; pidx++)
                {
                    int edge = pidx / n;
                    int offset = pidx % n;

                    FVector2D P;

                    switch (edge)
                    {
                    case 0: // upper (left → right)
                        P = FVector2D(Start.X + float(offset) * Step, Start.Y);
                        break;
                    case 1: // right (top → bottom)
                        P = FVector2D(Start.X + Size, Start.Y + float(offset) * Step);
                        break;
                    case 2: // bottom (right → left)
                        P = FVector2D(Start.X + Size - float(offset) * Step, Start.Y + Size);
                        break;
                    case 3: // left (bottom → top)
                        P = FVector2D(Start.X, Start.Y + Size - float(offset) * Step);
                        break;
                    }
                    float dist = FVector2D::Distance(P, Center);

                    float u = (P.X - cx) / fx;
                    float v = -(P.Y - cy) / fy;
                    float r = FMath::Sqrt(u * u + v * v);
                    float lambda = FMath::Atan2(v, u);

                    float Exponent = 4.0f; // Adjustable; larger values result in faster attenuation
                    float radiusNorm = dist / Radius;
                    float SaturatedR = FMath::Clamp(radiusNorm, 0.0f, 1.0f);
                    float FadeFactor = 1.0f - FMath::Pow(SaturatedR, Exponent);

                    if (SmallerAndEqual(Radius, dist, EPS))
                    {
                        continue;
                    }

                    // Newton's method to solve for theta
                    float theta = r;
                    bool res = true;
                    res = SolveThetaNewton(r, d1, d2, d3, d4, r, 0.0f, theta_f, theta);

                    if (!(SmallerAndEqual(0.0f, theta, EPS) && SmallerAndEqual(theta, theta_f, EPS)) || !res)
                    {
                        continue;
                    }
                    float FOVw = 1.0 - FMath::SmoothStep(theta_f - 0.2, theta_f, theta);
                    (*FisheyeMaskptr)[j * Resolution.X + i] = FOVw * FadeFactor;

                    FVector OPNormal = FVector(FMath::Cos(theta), FMath::Sin(theta) * FMath::Cos(lambda), FMath::Sin(theta) * FMath::Sin(lambda));
                    OPNormal.Normalize();

                    for (int m = 0; m < PlaneArray.Num(); m++)
                    {
                        // Intersection point in normalized spatial coordinates, note that the plane is a 2x2 plane at this time.
                        FVector IntersectPointNormal;
                        bool Intersect = RayPlaneIntersection(FVector::ZeroVector, OPNormal, PlaneArray[m], IntersectPointNormal);
                        bool InRange = IsInRange(IntersectPointNormal);
                        float Angle = FMath::Acos(FVector::DotProduct(IntersectPointNormal.GetSafeNormal(), FVector(1.0f, 0.0f, 0.0f)));
                        bool IsInFOV = Angle <= (FMath::DegreesToRadians(FOV * 0.5f)) ? true : false;
                        if (Intersect && InRange && IsInFOV)
                        {
                            if (Input.Num() == 0)
                            {
                                Input.Add(FPointInfo(IntersectPointNormal, { m }));
                            }
                            else
                            {
                                if (Input.Last().WorldPos.Equals(IntersectPointNormal, EPS))
                                {
                                    Input.Last().FaceIndex.Add(m);
                                }
                                else
                                {
                                    Input.Add(FPointInfo(IntersectPointNormal, { m }));
                                }
                            }
                        }
                    }
                }

                if (Input.Num() > 0)
                {
                    OutGroups.SetNum(SnitchNum);
                    SplitPoints(i, j, Input, OutGroups);
                    check(OutGroups.Num());
                    if (OutGroups.Num() > 0)
                    {
                        TArray<TMap<FIntVector, float>> AllPixelMap;
                        for (int groupidx = 0; groupidx < OutGroups.Num(); groupidx++)
                        {
                            TMap<FIntVector, float>& PixelMap = AllPixelMap.AddDefaulted_GetRef();
                            if (OutGroups[groupidx].Num() == 0)
                                continue;
                            int AABBXMin = INT_MAX;
                            int AABBXMax = INT_MIN;
                            int AABBYMin = INT_MAX;
                            int AABBYMax = INT_MIN;
                            for (int pidx = 0; pidx < OutGroups[groupidx].Num(); pidx++)
                            {
                                const FVector& P = OutGroups[groupidx][pidx];
                                AABBXMin = FMath::Min(AABBXMin, FMath::FloorToInt(P.X));
                                AABBXMax = FMath::Max(AABBXMax, FMath::CeilToInt(P.X));
                                AABBYMin = FMath::Min(AABBYMin, FMath::FloorToInt(P.Y));
                                AABBYMax = FMath::Max(AABBYMax, FMath::CeilToInt(P.Y));
                            }
                            float Area = ComputePolygonArea2D(OutGroups[groupidx]);

                            TArray<FIntVector> PixelArray;
                            float MipLV = FMath::Max(0.0f, FMath::Log2(Area) / 2);
                            int MaxMipValue = (SnitchNum == 4) ? 3 : 1;
                            int FloorMipLV = FMath::Clamp(FMath::FloorToInt(MipLV), 0, MaxMipValue);
                            int CeilMipLV = FMath::Clamp(FMath::CeilToInt(MipLV), 0, MaxMipValue);
                            for (int x = AABBXMin; x < AABBXMax; x++)
                            {
                                for (int y = AABBYMin; y < AABBYMax; y++)
                                {
                                    float Percentage = 0.0f;
                                    for (int sampleidx = 0; sampleidx < HaltonPoints.Num(); sampleidx++)
                                    {
                                        FVector2D samplep = FVector2D(float(x), float(y)) + HaltonPoints[sampleidx];
                                        if (IsPointInPolygon(samplep, OutGroups[groupidx]))
                                        {
                                            Percentage += 1.0 / 15.0;
                                        }
                                    }
                                    if (Percentage > 0.0f)
                                    {
                                        PixelArray.Add(FIntVector(FMath::FloorToInt(x), FMath::FloorToInt(y), 0));
                                        PixelMap.Add(FIntVector(FMath::FloorToInt(x), FMath::FloorToInt(y), 0), Percentage);
                                    }
                                }
                            }

                            // Merge pixels and compress them into a higher-level mipmap
                            TArray<FIntVector> PixelArrayToAdd;
                            TSet<FIntVector> PixelArrayToRemove;
                            TMap<FIntVector, float> PixelMapToAdd;

                            // First pass: collect elements to add/remove
                            for (int l = 0; l < PixelArray.Num(); ++l)
                            {
                                const FIntVector& CurrentPix = PixelArray[l];

                                if (PixelArrayToRemove.Contains(CurrentPix) || CurrentPix.Z != 0)
                                    continue;

                                for (int m = CeilMipLV; m >= FloorMipLV; m--)
                                {
                                    FIntPoint MipCoord = GetMipmapCoord(FIntVector(CurrentPix.X, CurrentPix.Y, m));
                                    int MipXMin = MipCoord.X << m;
                                    int MipXMax = (MipCoord.X + 1) << m;
                                    int MipYMin = MipCoord.Y << m;
                                    int MipYMax = (MipCoord.Y + 1) << m;

                                    float Weight = 0.0f;
                                    for (int mipx = MipXMin; mipx < MipXMax; mipx++)
                                    {
                                        for (int mipy = MipYMin; mipy < MipYMax; mipy++)
                                        {
                                            FIntVector SubPix(mipx, mipy, 0);
                                            if (PixelMap.Contains(SubPix))
                                            {
                                                Weight += PixelMap[SubPix];
                                            }
                                        }
                                    }

                                    float ThresholdWeight = 0.6f * FMath::Pow(4, m);
                                    if (Weight >= ThresholdWeight)
                                    {
                                        FIntVector MipPix(MipCoord.X, MipCoord.Y, m);

                                        PixelArrayToAdd.Add(MipPix);
                                        PixelMapToAdd.Add(MipPix, Weight);

                                        for (int mipx = MipXMin; mipx < MipXMax; mipx++)
                                        {
                                            for (int mipy = MipYMin; mipy < MipYMax; mipy++)
                                            {
                                                FIntVector SubPix(mipx, mipy, 0);
                                                PixelArrayToRemove.Add(SubPix);
                                            }
                                        }
                                        break;
                                    }
                                }
                            }

                            // Second step: Apply all changes
                            for (const FIntVector& Pix : PixelArrayToRemove)
                            {
                                PixelMap.Remove(Pix);
                                PixelArray.Remove(Pix);
                            }

                            for (const FIntVector& Pix : PixelArrayToAdd)
                            {
                                PixelArray.Add(Pix);
                            }

                            for (const TPair<FIntVector, float>& Pair : PixelMapToAdd)
                            {
                                PixelMap.Add(Pair.Key, Pair.Value);
                            }

                        }

                        TArray<FPixelInfo> AllPixels;
                        // Traverse all faces
                        for (int32 GroupIdx = 0; GroupIdx < AllPixelMap.Num(); GroupIdx++)
                        {
                            TMap<FIntVector, float>& PixelMap = AllPixelMap[GroupIdx];
                            for (TPair<FIntVector, float>& Pair : PixelMap)
                            {
                                FIntVector& Key = Pair.Key;
                                float Weight = Pair.Value;
                                int32 X = Key.X;
                                int32 Y = Key.Y;
                                int32 MipLevel = Key.Z;
                                AllPixels.Add(FPixelInfo(GroupIdx, X, Y, MipLevel, Weight));
                            }
                        }

                        // Sort: Arrange in descending order by weight.
                        AllPixels.Sort([](const FPixelInfo& A, const FPixelInfo& B)
                        {
                            return A.Weight > B.Weight;
                        });

                        int res;
                        int weight4;
                        for (int k = 0; k < TopNPixel; k++)
                        {
                            if (k >= AllPixels.Num())
                            {
                                if (SnitchNum == 4)
                                {
                                    PackToInt32(res, 0, 0, 0, 0, 0);
                                }
                                else if (SnitchNum == 5)
                                {
                                    PackToInt32_Tex3Bit(res, 0, 0, 0, 0, 0);
                                }
                            }
                            else
                            {
                                weight4 = FMath::Clamp(FMath::RoundToInt(AllPixels[k].Weight * 15.0f), 0, 15);
                                if (SnitchNum == 4)
                                {
                                    PackToInt32(res, AllPixels[k].TextureIndex, AllPixels[k].MipLevel, AllPixels[k].X, AllPixels[k].Y, weight4);
                                }
                                else if (SnitchNum == 5)
                                {
                                    PackToInt32_Tex3Bit(res, AllPixels[k].TextureIndex, AllPixels[k].MipLevel, AllPixels[k].X, AllPixels[k].Y, weight4);
                                }
                            }
                            (*SamplePanelIDptr)[(j * Resolution.X + i) * TopNPixel + k] = res;
                        }
                    }
                }
            });

            // Store lookup table
            FString IDFileName = TEXT("ID_") + ID + TEXT(".bin");
            FString IDFilePath = FPaths::ProjectSavedDir() / IDFileName;
            FString MaskFileName = TEXT("Mask_") + ID + TEXT(".bin");
            FString MaskFilePath = FPaths::ProjectSavedDir() / MaskFileName;

            if (SaveResourceArrayToFile(IDFilePath, *SamplePanelIDptr))
            {
                UE_LOG(LogTemp, Log, TEXT("Saved file: %s"), *IDFilePath);
            }
            else
            {
                UE_LOG(LogTemp, Error, TEXT("Failed to save %s."), *IDFilePath);
                return;
            }

            if (SaveResourceArrayToFile(MaskFilePath, *FisheyeMaskptr))
            {
                UE_LOG(LogTemp, Log, TEXT("Saved file: %s"), *MaskFilePath);
            }
            else
            {
                UE_LOG(LogTemp, Error, TEXT("Failed to save %s."), *MaskFilePath);
                return;
            }
        }
        FRHIResourceCreateInfo* CreateInfoPtr = new FRHIResourceCreateInfo();
        CreateInfoPtr->ResourceArray = SamplePanelIDptr.Get();

        FStructuredBufferRHIRef Buffer = RHICreateStructuredBuffer(
            sizeof(int),
            sizeof(int) * SamplePanelIDptr->Num(),
            BUF_Static | BUF_ShaderResource,
            *CreateInfoPtr
        );

        FShaderResourceViewRHIRef SRV = RHICreateShaderResourceView(Buffer);

        MapSamplePanelIDBuffer.Add(ID, Buffer);
        MapSamplePanelIDSRV.Add(ID, SRV);
        MapCreateInfoSamplePanelID.Add(ID, CreateInfoPtr);

        FRHIResourceCreateInfo* FisheyeMaskCreateInfoPtr = new FRHIResourceCreateInfo();
        FisheyeMaskCreateInfoPtr->ResourceArray = FisheyeMaskptr.Get();

        FStructuredBufferRHIRef FisheyeMaskBuffer = RHICreateStructuredBuffer(
            sizeof(float),
            sizeof(float) * FisheyeMaskptr->Num(),
            BUF_Static | BUF_ShaderResource,
            *FisheyeMaskCreateInfoPtr);
        FShaderResourceViewRHIRef FisheyeMaskSRV = RHICreateShaderResourceView(FisheyeMaskBuffer);

        MapFisheyeMaskBuffer.Add(ID, FisheyeMaskBuffer);
        MapFisheyeMaskSRV.Add(ID, FisheyeMaskSRV);
        MapFisheyeMaskCreateInfo.Add(ID, FisheyeMaskCreateInfoPtr);
    }
}

FIntPoint UFisheyeCS4CameraRendering::GetMipmapCoord(FIntVector Coord)
{
    return FIntPoint(Coord.X >> Coord.Z, Coord.Y >> Coord.Z);
}


bool UFisheyeCS4CameraRendering::IsPointInPolygon(FVector2D& Point, TArray<FVector>& Polygon)
{
    int numIntersections = 0;
    int numPoints = Polygon.Num();

    for (int i = 0; i < numPoints; ++i)
    {
        FVector2D A;
        A.X = Polygon[i].X;
        A.Y = Polygon[i].Y;
        FVector2D B;
        B.X = Polygon[(i + 1) % numPoints].X;
        B.Y = Polygon[(i + 1) % numPoints].Y;

        // Determine whether it crosses the horizontal line y = y0.
        if ((A.Y > Point.Y) != (B.Y > Point.Y))
        {
            float intersectX = A.X + (Point.Y - A.Y) * (B.X - A.X) / (B.Y - A.Y);
            if (Point.X < intersectX)
            {
                numIntersections++;
            }
        }
    }
    return (numIntersections % 2) == 1;
}


bool UFisheyeCS4CameraRendering::IsSampleInCircle(float i, float j, FIntPoint Resolution)
{
    FVector2D SamplePoint(i, j);

    FVector2D ImageCenter(Resolution.X / 2, Resolution.Y / 2);
    float Dist = (ImageCenter - SamplePoint).Size();
    return Dist <= ImageCenter.X;
}

bool UFisheyeCS4CameraRendering::RayPlaneIntersection(FVector RayOrigin, FVector RayDirection, FPlane Plane, FVector& OutHitPoint)
{
    FVector PlaneNormal = FVector(Plane.X, Plane.Y, Plane.Z);
    FVector PlaneOrigin = GetRandomPointOnPlane(Plane);

    float Denominator = FVector::DotProduct(RayDirection, PlaneNormal);

    if (FMath::IsNearlyZero(Denominator, EPS))
    {
        return false;
    }

    float t = FVector::DotProduct((PlaneOrigin - RayOrigin), PlaneNormal) / Denominator;

    if (t < 0)
    {
        return false;
    }

    OutHitPoint = RayOrigin + RayDirection * t;
    return true;
}


// Note: For computational convenience, the plane dimensions are set to 2x2 so that
// the face coordinates are bounded within [-1,1]. Therefore, the calculated image
// coordinate range here is [0,0] to [2,2]. To convert to the UV coordinate system,
// divide by 2.
FVector UFisheyeCS4CameraRendering::LocalSpace2Panel(int PanelID, FVector IntersectPoint)
{
    FMatrix TranslationMatrix;
    FMatrix RotationMatrix;
    FMatrix M;
    if(SnitchNum == 4)
    {
        switch (PanelID)
        {
            //left
        case 0:
            TranslationMatrix = FTranslationMatrix(FVector(0.0f, -UE_SQRT_2, 1.0f) * Radius);
            RotationMatrix = FRotationMatrix::MakeFromXY(
                FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.0f),
                FVector(0.0f, 0.0f, -1.0f));
            break;
            //right
        case 1:
            TranslationMatrix = FTranslationMatrix(FVector(UE_SQRT_2, 0.0f, 1.0f) * Radius);
            RotationMatrix = FRotationMatrix::MakeFromXY(
                FVector(-UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.0f),
                FVector(0, 0.0f, -1.0f));
            break;
            //top, y = 1
        case 2:
            TranslationMatrix = FTranslationMatrix(FVector(-UE_SQRT_2, 0.0f, 1.0f) * Radius);
            RotationMatrix = FRotationMatrix::MakeFromXY(
                FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.0f),
                FVector(UE_SQRT_2 / 2, -UE_SQRT_2, 0.0f));
            break;
            //    //bottom, z = 1
        case 3:
            TranslationMatrix = FTranslationMatrix(FVector(UE_SQRT_2, 0.0, -1.0f) * Radius);
            RotationMatrix = FRotationMatrix::MakeFromXY(
                FVector(-UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.0f),
                FVector(-UE_SQRT_2 / 2, -UE_SQRT_2 / 2, 0.0f));
            break;
        }
    }else if(SnitchNum == 5)
    {
        switch (PanelID)
        {
        case 0 :
            TranslationMatrix = FTranslationMatrix(FVector(1.0f, -1.0f, 1.0f) * Radius);
            RotationMatrix = FRotationMatrix::MakeFromXY(
                FVector(0.0f, 1.0f, 0.0f),
                FVector(0.0f, 0.0f, -1.0f));
            break;
        case 1:
            TranslationMatrix = FTranslationMatrix(FVector(-1.0f, -1.0f, 1.0f) * Radius);
            RotationMatrix = FRotationMatrix::MakeFromXY(
                FVector(1.0f, 0.0f, 0.0f),
                FVector(0.0f, 0.0f, -1.0f));
            break;
        case 2:
            TranslationMatrix = FTranslationMatrix(FVector(1.0f, 1.0f, 1.0f) * Radius);
            RotationMatrix = FRotationMatrix::MakeFromXY(
                FVector(-1.0f, 0.0f, 0.0f),
                FVector(0.0f, 0.0f, -1.0f));
            break;
        case 3:
            TranslationMatrix = FTranslationMatrix(FVector(-1.0f, -1.0f, 1.0f) * Radius);
            RotationMatrix = FRotationMatrix::MakeFromXY(
                FVector(0.0f, 1.0f, 0.0f),
                FVector(1.0f, 0.0f, 0.0f));
            break;
        case 4:
            TranslationMatrix = FTranslationMatrix(FVector(1.0f, -1.0f, -1.0f) * Radius);
            RotationMatrix = FRotationMatrix::MakeFromXY(
                FVector(0.0f, 1.0f, 0.0f),
                FVector(-1.0f, 0.0f, 0.0f));
            break;
        }
    }

    M = RotationMatrix * TranslationMatrix;
    M = M.Inverse();
    FVector4 Res = M.TransformPosition(IntersectPoint);
    return FVector(Res.X, Res.Y, Res.Z);
}

bool UFisheyeCS4CameraRendering::SmallerAndEqual(float A, float B, float eps = EPS)
{
    return FMath::IsNearlyEqual(A, B, eps) || (A < B);
}

FVector UFisheyeCS4CameraRendering::GetRandomPointOnPlane(const FPlane& Plane)
{
    FVector PlaneNormal(Plane.X, Plane.Y, Plane.Z);

    // Assume set x = 0, y = 0 to solve for z
    if (!FMath::IsNearlyZero(PlaneNormal.Z)) {
        float z = Plane.W / PlaneNormal.Z;
        return FVector(0, 0, z); 
    }

    if (!FMath::IsNearlyZero(PlaneNormal.Y)) {
        float y = Plane.W / PlaneNormal.Y;
        return FVector(0, y, 0); 
    }

    float x = Plane.W / PlaneNormal.X;
    return FVector(x, 0, 0); 
}

bool UFisheyeCS4CameraRendering::IsInRange(FVector Point)
{
    float x = Point.X, y = Point.Y, z = Point.Z;
    if(SnitchNum == 4)
    {
        if (SmallerAndEqual(-1.0f * Radius, z) && SmallerAndEqual(z, 1.0f * Radius) && SmallerAndEqual(0.0f, x))
        {
            if (SmallerAndEqual(-UE_SQRT_2 * Radius, y) && SmallerAndEqual(y, 0.0f))
            {
                if (SmallerAndEqual(0.0f, x) && SmallerAndEqual(x, y + UE_SQRT_2 * Radius))
                {
                    return true;
                }
            }
            else if (SmallerAndEqual(0.0f, y) && SmallerAndEqual(y, UE_SQRT_2 * Radius))
            {
                if (SmallerAndEqual(0.0f, x) && SmallerAndEqual(x, -y + UE_SQRT_2 * Radius))
                {
                    return true;
                }
            }
        }
    }else if(SnitchNum == 5)
    {
        bool RangeX = SmallerAndEqual(-1.0f * Radius, x) && SmallerAndEqual(x, 1.0f * Radius);
        bool RangeY = SmallerAndEqual(-1.0f * Radius, y) && SmallerAndEqual(y, 1.0f * Radius);
        bool RangeZ = SmallerAndEqual(-1.0f * Radius, z) && SmallerAndEqual(z, 1.0f * Radius);
        if (RangeX && RangeY && RangeZ)
        {
            bool OnNegativeY = FMath::IsNearlyEqual(y, -1.0f * Radius, EPS);
            bool OnPositiveY = FMath::IsNearlyEqual(y, 1.0f * Radius, EPS);
            bool OnNegativeZ = FMath::IsNearlyEqual(z, -1.0f * Radius, EPS);
            bool OnPositiveZ = FMath::IsNearlyEqual(z, 1.0f * Radius, EPS);
            bool OnPositiveX = FMath::IsNearlyEqual(x, 1.0f * Radius, EPS);
            if(OnNegativeY || OnPositiveY || OnNegativeZ || OnPositiveZ || OnPositiveX)
            {
                return true;
            }
            
        }
    }
    return false;
}

void UFisheyeCS4CameraRendering::BeginDestroy()
{
    Super::BeginDestroy();
}


#undef LOCTEXT_NAMESPACE
#pragma optimize("", on)