#include "FisheyeCS4CameraRendering.h"
#include <cassert>

#include "FileManager.h"
#include "Containers/DynamicRHIResourceArray.h"
#include "Engine/Classes/Engine/TextureRenderTarget2D.h"  
#include "Engine/Classes/Engine/World.h"  
#include "RenderCore/Public/GlobalShader.h"  
#include "RHI/Public/PipelineStateCache.h"  
#include "RHI/Public/RHIStaticStates.h"  
#include "Engine/Public/SceneUtils.h"  
#include "Engine/Public/SceneInterface.h"  
#include "RenderCore/Public/ShaderParameterUtils.h"  
#include "Core/Public/Logging/MessageLog.h"  
#include "Core/Public/Internationalization/Internationalization.h"  
#include "Runtime/Engine/Classes/Engine/Texture2D.h"
#include "Runtime/RenderCore/Public/RenderGraph.h"
#include "Runtime/RenderCore/Public/RenderGraphUtils.h"
#include "Runtime/RenderCore/Public/RenderTargetPool.h"

#define NUM_THREADS_PER_GROUP_DIMENSION 32

#pragma optimize("", off)
#define LOCTEXT_NAMESPACE "FisheyeCS4Camera"

TMap<FString, TSharedPtr<TResourceArray<int>>> UFisheyeCS4CameraRendering::MapSamplePanelID;
TMap<FString, FStructuredBufferRHIRef> UFisheyeCS4CameraRendering::MapSamplePanelIDBuffer;
TMap<FString, FShaderResourceViewRHIRef> UFisheyeCS4CameraRendering::MapSamplePanelIDSRV;
TMap<FString, FRHIResourceCreateInfo*> UFisheyeCS4CameraRendering::MapCreateInfoSamplePanelID;

TMap<int32, TSharedPtr<TResourceArray<int>>> UFisheyeCS4CameraRendering::MapFisheyeMask;
TMap<int32, FStructuredBufferRHIRef> UFisheyeCS4CameraRendering::MapFisheyeMaskBuffer;
TMap<int32, FShaderResourceViewRHIRef> UFisheyeCS4CameraRendering::MapFisheyeMaskSRV;
TMap<int32, FRHIResourceCreateInfo*> UFisheyeCS4CameraRendering::MapFisheyeMaskCreateInfo;


float EPSINON = 0.00001;

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
    //UE_LOG(LogTemp, Warning, TEXT("djw debug :ClampedKernelRadius %f,IntegerKernelRadius %d"), 
	    //ClampedKernelRadius, IntegerKernelRadius);
    uint32 SampleCount = 0;
    float WeightSum = 0.0f;

    for (int32 SampleIndex = -IntegerKernelRadius; SampleIndex <= IntegerKernelRadius; SampleIndex++)
    {
        float Weight = NormalDistributionUnscaled(SampleIndex, ClampedKernelRadius);
        //UE_LOG(LogTemp, Warning, TEXT("djw debug :SampleIndex %d Weight %f"), SampleIndex,Weight);
        Gaussian1dKernel.Add(Weight);
        WeightSum += Weight;
        SampleCount++;
    }

    float WeightSumInverse = 1.0f / WeightSum;
    for (uint32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
    {
        Gaussian1dKernel[SampleIndex] *= WeightSumInverse;
        //UE_LOG(LogTemp, Warning, TEXT("djw debug :after norm SampleIndex %d Weight %f"), SampleIndex, Gaussian1dKernel[SampleIndex]);
    }
}


// Pack three integer values into a single int32_t
void UFisheyeCS4CameraRendering::PackToInt32(int &res, int high4, int mid14, int low14) {
    assert(high4 >= 0 && high4 < (1 << 4));    // Ensure high4 fits in 4 bits
    assert(mid14 >= 0 && mid14 < (1 << 14));   // Ensure mid14 fits in 14 bits
    assert(low14 >= 0 && low14 < (1 << 14));   // Ensure low14 fits in 14 bits

    res = (high4 << 28) | (mid14 << 14) | low14;
}

// Unpack three values from a single int32_t
void UFisheyeCS4CameraRendering::UnpackFromInt32(int packed, int &high4, int &mid14, int &low14) {
    high4 = (packed >> 28) & 0xF;       // Extract high 4 bits
    mid14 = (packed >> 14) & 0x3FFF;    // Extract middle 14 bits
    low14 = packed & 0x3FFF;            // Extract low 14 bits
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
    }

    void SetParameters(
        FRHICommandListImmediate& RHICmdList,
        TArray<TRefCountPtr<FRHITexture>> InputTextureRef,
        FTextureRHIRef& OutTextureRef,
        FUnorderedAccessViewRHIRef& OutputTextureUAVRef,
        FSamplerStateRHIRef SamplerState,
        FShaderResourceViewRHIRef& SamplePanelIDSRV)
    {
        for (int i = 0; i < InputTextureRef.Num(); i++)
        {
            if (InputTextureRef.IsValidIndex(i))
            {
                RHICmdList.SetShaderTexture(GetComputeShader(), InputTexture.GetBaseIndex() + i + 1, InputTextureRef[i]);
            }
            else
            {
                UE_LOG(LogTemp, Error, TEXT("InputTextureRef.IsValidIndex(%d)"), i);
            }
        }
        RWOutputTexture.SetTexture(RHICmdList, GetComputeShader(), OutTextureRef, OutputTextureUAVRef);
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
        return bShaderHasOutdatedParameters;
    }

private:
    FShaderResourceParameter InputTexture;
    FRWShaderParameter RWOutputTexture;
    FShaderResourceParameter SamplePanelID;
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
        // 绑定输入纹理（只读 SRV）
        InputMipmapTexture.Bind(Initializer.ParameterMap, TEXT("InputMipmapTexture"));
        // 绑定输出纹理（可写 UAV）
        RWOutputMipmapTexture.Bind(Initializer.ParameterMap, TEXT("RWOutputMipmapTexture"));
        InputTextureSampler.Bind(Initializer.ParameterMap, TEXT("InputTextureSampler"));
    }

    // 设置着色器参数（输入 SRV 和输出 UAV）
    void SetParameters(
        FRHICommandListImmediate& RHICmdList,
        FShaderResourceViewRHIRef& InputTextureSRV,
        FUnorderedAccessViewRHIRef& OutputTextureUAV,
        FSamplerStateRHIRef& SamplerState)
    {
        // 设置输入纹理的 SRV
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), InputMipmapTexture.GetBaseIndex(), InputTextureSRV);
        // 设置输出纹理的 UAV
        RHICmdList.SetUAVParameter(GetComputeShader(), RWOutputMipmapTexture.GetUAVIndex(), OutputTextureUAV);
        RHICmdList.SetShaderSampler(GetComputeShader(), InputTextureSampler.GetBaseIndex(), SamplerState);
    }

    // 仅支持 SM5 特性级别
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    // 修改编译环境（可选）
    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    }

    // 序列化参数（保存和加载）
    virtual bool Serialize(FArchive& Ar) override
    {
        bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
        Ar << InputMipmapTexture;
        Ar << RWOutputMipmapTexture;
        Ar << InputTextureSampler;
        return bShaderHasOutdatedParameters;
    }

private:
    // 输入纹理（只读 SRV）
    FShaderResourceParameter InputMipmapTexture;
    // 输出纹理（可写 UAV）
    FRWShaderParameter RWOutputMipmapTexture;
    // 采样器
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
        // 绑定输入纹理（只读 SRV）
        InputTexture.Bind(Initializer.ParameterMap, TEXT("InputGaussBlurTexture"));
        // 绑定输出纹理（可写 UAV）
        RWOutputTexture.Bind(Initializer.ParameterMap, TEXT("RWOutputGaussBlurTexture"));
        // 绑定高斯模糊一维核
        GaussBlurKernel1d.Bind(Initializer.ParameterMap, TEXT("GaussBlurKernel1d"));
        // 绑定高斯模糊一维核长度
        BlurLength.Bind(Initializer.ParameterMap, TEXT("BlurLength"));
        // 绑定模糊方向（0=水平，1=垂直）
        BlurDirection.Bind(Initializer.ParameterMap, TEXT("BlurDirection"));
        Sampler.Bind(Initializer.ParameterMap, TEXT("Sampler"));
    }

    // 设置着色器参数（输入 SRV、输出 UAV 和方向）
    void SetParameters(
        FRHICommandListImmediate& RHICmdList,
        FShaderResourceViewRHIRef& InputTextureSRV,
        FUnorderedAccessViewRHIRef& OutputTextureUAV,
        FShaderResourceViewRHIRef& GaussBlur1dSRV,
        int32 Length,
        int32 Direction,    // 新增参数：模糊方向
        FSamplerStateRHIRef& SamplerState) 
    {
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), InputTexture.GetBaseIndex(), InputTextureSRV);
        RHICmdList.SetUAVParameter(GetComputeShader(), RWOutputTexture.GetUAVIndex(), OutputTextureUAV);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), GaussBlurKernel1d.GetBaseIndex(), GaussBlur1dSRV);
        SetShaderValue(RHICmdList, GetComputeShader(), BlurLength, Length);
        SetShaderValue(RHICmdList, GetComputeShader(), BlurDirection, Direction);
        RHICmdList.SetShaderSampler(GetComputeShader(), Sampler.GetBaseIndex(), SamplerState);
    }

    // 仅支持 SM5 特性级别
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    // 修改编译环境（可选）
    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    }

    // 序列化参数（保存和加载）
    virtual bool Serialize(FArchive& Ar) override
    {
        bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
        Ar << InputTexture;
        Ar << RWOutputTexture;
        Ar << GaussBlurKernel1d;
        Ar << BlurLength;
        Ar << BlurDirection; // 序列化新增的方向参数
        Ar << Sampler;
        return bShaderHasOutdatedParameters;
    }

private:
    // 输入纹理（只读 SRV）
    FShaderResourceParameter InputTexture;
    // 输出纹理（可写 UAV）
    FRWShaderParameter RWOutputTexture;
    // 高斯模糊一维核
    FShaderResourceParameter GaussBlurKernel1d;
    //高斯模糊一维核长度
    FShaderParameter BlurLength;
    // 模糊方向（0=水平，1=垂直）
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
        // 绑定输入纹理（只读 SRV）
        InputTexture.Bind(Initializer.ParameterMap, TEXT("InputGaussBlurTexture"));
        InputAddTexture.Bind(Initializer.ParameterMap, TEXT("InputAddTexture"));
        // 绑定输出纹理（可写 UAV）
        RWOutputTexture.Bind(Initializer.ParameterMap, TEXT("RWOutputGaussBlurTexture"));
        // 绑定高斯模糊一维核
        GaussBlurKernel1d.Bind(Initializer.ParameterMap, TEXT("GaussBlurKernel1d"));
        // 绑定高斯模糊一维核长度
        BlurLength.Bind(Initializer.ParameterMap, TEXT("BlurLength"));
        // 绑定模糊方向（0=水平，1=垂直）
        BlurDirection.Bind(Initializer.ParameterMap, TEXT("BlurDirection"));
        Sampler.Bind(Initializer.ParameterMap, TEXT("Sampler"));
        AddSampler.Bind(Initializer.ParameterMap, TEXT("AddSampler"));
    }

    // 设置着色器参数（输入 SRV、输出 UAV 和方向）
    void SetParameters(
        FRHICommandListImmediate& RHICmdList,
        FShaderResourceViewRHIRef& InputTextureSRV,
        FShaderResourceViewRHIRef& InputAddTextureSRV,
        FUnorderedAccessViewRHIRef& OutputTextureUAV,
        FShaderResourceViewRHIRef& GaussBlur1dSRV,
        int32 Length,
        int32 Direction,    // 新增参数：模糊方向
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

    // 仅支持 SM5 特性级别
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    // 修改编译环境（可选）
    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    }

    // 序列化参数（保存和加载）
    virtual bool Serialize(FArchive& Ar) override
    {
        bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
        Ar << InputTexture;
        Ar << InputAddTexture;
        Ar << RWOutputTexture;
        Ar << GaussBlurKernel1d;
        Ar << BlurLength;
        Ar << BlurDirection; // 序列化新增的方向参数
        Ar << Sampler;
        Ar << AddSampler;
        return bShaderHasOutdatedParameters;
    }

private:
    // 输入纹理（只读 SRV）
    FShaderResourceParameter InputTexture;
    // 输入的additive纹理（只读 SRV）
    FShaderResourceParameter InputAddTexture;
    // 输出纹理（可写 UAV）
    FRWShaderParameter RWOutputTexture;
    // 高斯模糊一维核
    FShaderResourceParameter GaussBlurKernel1d;
    //高斯模糊一维核长度
    FShaderParameter BlurLength;
    // 模糊方向（0=水平，1=垂直）
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
        // 绑定输入纹理（只读 SRV）
        InputOriTexture.Bind(Initializer.ParameterMap, TEXT("InputOriTexture"));
        InputBlurTexture.Bind(Initializer.ParameterMap, TEXT("InputBlurTexture"));
        InputLUTTexture.Bind(Initializer.ParameterMap, TEXT("InputLUTTexture"));
        // 绑定输出纹理（可写 UAV）
        RWOutputTexture.Bind(Initializer.ParameterMap, TEXT("RWOutputTexture"));
        Sampler.Bind(Initializer.ParameterMap, TEXT("Sampler"));
        FisheyeMask.Bind(Initializer.ParameterMap, TEXT("FisheyeMask"));
    }

    // 设置着色器参数（输入 SRV 和输出 UAV）
    void SetParameters(
        FRHICommandListImmediate& RHICmdList,
        FShaderResourceViewRHIRef& InputHighResOri,
        FShaderResourceViewRHIRef& InputLowResBlur,
        FShaderResourceViewRHIRef& InputLUT,
        FUnorderedAccessViewRHIRef& OutputUpscaled,
        FSamplerStateRHIRef& SamplerState,
        FShaderResourceViewRHIRef& FisheyeMaskSRV)
    {
        // 设置输入纹理的 SRV
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), InputOriTexture.GetBaseIndex(), InputHighResOri);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), InputBlurTexture.GetBaseIndex(), InputLowResBlur);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), InputLUTTexture.GetBaseIndex(), InputLUT);
        // 设置输出纹理的 UAV
        RHICmdList.SetUAVParameter(GetComputeShader(), RWOutputTexture.GetUAVIndex(), OutputUpscaled);
        RHICmdList.SetShaderSampler(GetComputeShader(), Sampler.GetBaseIndex(), SamplerState);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), FisheyeMask.GetBaseIndex(), FisheyeMaskSRV);
    }

    // 仅支持 SM5 特性级别
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    // 修改编译环境（可选）
    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    }

    // 序列化参数（保存和加载）
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
    // 输入原图纹理（只读 SRV）
    FShaderResourceParameter InputOriTexture;
    // 输入模糊图纹理（只读 SRV）:
    FShaderResourceParameter InputBlurTexture;
    // LUT纹理（只读 SRV）:
    FShaderResourceParameter InputLUTTexture;
    // 输出纹理（可写 UAV）
    FRWShaderParameter RWOutputTexture;
    // 采样器
    FShaderResourceParameter Sampler;
    // 遮挡
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

    // 设置着色器参数（输入 SRV 和输出 UAV）
    void SetParameters(
        FRHICommandListImmediate& RHICmdList,
        FUnorderedAccessViewRHIRef& LUTTextureRef)
    {
        RHICmdList.SetUAVParameter(GetComputeShader(), LUTTexture.GetUAVIndex(), LUTTextureRef);
    }

    // 仅支持 SM5 特性级别
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    // 修改编译环境（可选）
    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
    }

    // 序列化参数（保存和加载）
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
    //FTexture2DRHIRef Texture = CreateLUT(RHICmdList);
    return Texture;
}

FTexture2DRHIRef UFisheyeCS4CameraRendering::CreateLUT(FRHICommandListImmediate& RHICmdList)
{
    check(IsInRenderingThread());
    uint32 GroupSize = 32;
    uint32 SizeX = 1024;
    uint32 SizeY = 32;

    //两个整数相除后向上取整
    uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)SizeX, GroupSize);
    uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)SizeY, GroupSize);

    FRHIResourceCreateInfo OutputInfo;
    FTexture2DRHIRef OutputRHITexture = RHICreateTexture2D(SizeX, SizeY,
        PF_B8G8R8A8, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, OutputInfo);
    //创建贴图资源的UAV视图
    FUnorderedAccessViewRHIRef OutputUAV = RHICreateUnorderedAccessView(OutputRHITexture);
    TRefCountPtr<FRHITexture> OutputTextureRef(OutputRHITexture);

    //创建贴图资源的SRV视图
    TShaderMapRef<FLUTTextureComputeShader> LUTComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    //选取FMipmapsComputeShader
    RHICmdList.SetComputeShader(LUTComputeShader->GetComputeShader());

    // 将参数传递给ComputeShader
    //这里我们实际上能用到的是UAV,追查到SetTexture函数我们可以发现，对于ComputeShader，第二个参数实际上是没有用的
    LUTComputeShader->SetParameters(RHICmdList, OutputUAV);

    //TransitionResource 是确保资源正确使用的关键函数，特别是在不同管线（如图形管线和计算管线）之间切换时。
    //它的作用是防止资源冲突并确保 GPU 按照预期顺序访问资源。在 Compute Shader 调用之前进行状态切换是标准流程，以避免访问未同步的资源数据。
    RHICmdList.TransitionResource(
        EResourceTransitionAccess::ERWNoBarrier,
        EResourceTransitionPipeline::EGfxToCompute,
        OutputUAV);

    DispatchComputeShader(RHICmdList, *LUTComputeShader, GroupSizeX, GroupSizeY, 1);
    //RHICmdList.CopyTexture(OutputRHITexture, OutputRHITexture, FRHICopyTextureInfo());
    return OutputRHITexture;
}

FTexture3DRHIRef UFisheyeCS4CameraRendering::CreateLUT3D(FRHICommandListImmediate& RHICmdList)
{
    check(IsInRenderingThread());
    uint32 GroupSize = 8;
    uint32 SizeX = 32;
    uint32 SizeY = 32;
    uint32 SizeZ = 32;

    //两个整数相除后向上取整
    uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)SizeX, GroupSize);
    uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)SizeY, GroupSize);
    uint32 GroupSizeZ = FMath::DivideAndRoundUp((uint32)SizeZ, GroupSize);

    FRHIResourceCreateInfo OutputInfo;
    FTexture3DRHIRef OutputRHITexture = RHICreateTexture3D(SizeX, SizeY, SizeZ,
        PF_B8G8R8A8, 1,  TexCreate_ShaderResource | TexCreate_UAV, OutputInfo);
    //创建贴图资源的UAV视图
    FUnorderedAccessViewRHIRef OutputUAV = RHICreateUnorderedAccessView(OutputRHITexture);
    TRefCountPtr<FRHITexture> OutputTextureRef(OutputRHITexture);

    //创建贴图资源的SRV视图
    TShaderMapRef<FLUTTextureComputeShader> LUTComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    //选取FMipmapsComputeShader
    RHICmdList.SetComputeShader(LUTComputeShader->GetComputeShader());

    // 将参数传递给ComputeShader
    //这里我们实际上能用到的是UAV,追查到SetTexture函数我们可以发现，对于ComputeShader，第二个参数实际上是没有用的
    LUTComputeShader->SetParameters(RHICmdList, OutputUAV);

    //TransitionResource 是确保资源正确使用的关键函数，特别是在不同管线（如图形管线和计算管线）之间切换时。
    //它的作用是防止资源冲突并确保 GPU 按照预期顺序访问资源。在 Compute Shader 调用之前进行状态切换是标准流程，以避免访问未同步的资源数据。
    RHICmdList.TransitionResource(
        EResourceTransitionAccess::ERWNoBarrier,
        EResourceTransitionPipeline::EGfxToCompute,
        OutputUAV);

    DispatchComputeShader(RHICmdList, *LUTComputeShader, GroupSizeX, GroupSizeY, GroupSizeZ);
    //RHICmdList.CopyTexture(OutputRHITexture, OutputRHITexture, FRHICopyTextureInfo());
    return OutputRHITexture;
}


void UFisheyeCS4CameraRendering::UseComputeShaderArray_RenderThread(
    FRHICommandListImmediate& RHICmdList,
    TArray<FTextureRenderTargetResource*> InTextureRenderTargetResource,
    FTextureRenderTargetResource* OutTextureRenderTargetResource,
    FTextureRenderTargetResource* MipBloomTextureRenderTargetResource0,
    FIntPoint Resolution,
    int SampleNum,
    int ProjectionModel,
    int layout)
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
            uint32 SizeX = InTextureRenderTargetResource[0]->GetSizeX();
            uint32 SizeY = InTextureRenderTargetResource[0]->GetSizeY();

            FIntPoint FullResolution = FIntPoint(SizeX, SizeY);
            //两个整数相除后向上取整
            uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)SizeX, GroupSize);
            uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)SizeY, GroupSize);

            //创建一个贴图资源
            FRHIResourceCreateInfo CreateInfo;
            FTexture2DRHIRef CreatedRHITexture = RHICreateTexture2D(SizeX, SizeY,
                PF_FloatRGBA, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, CreateInfo);
            //创建贴图资源的UAV视图
            FUnorderedAccessViewRHIRef TextureUAV = RHICreateUnorderedAccessView(CreatedRHITexture);
            TRefCountPtr<FRHITexture> OutputTextureRef(CreatedRHITexture);

            TArray<TRefCountPtr<FRHITexture>> InputTextureRef;
            for (int i = 0; i < InRenderTargetTexture.Num(); i++)
            {
                InputTextureRef.Add(TRefCountPtr<FRHITexture>(InRenderTargetTexture[i]));
            }

            static uint32 Count = 0;
            static TShaderMapRef<FFisheyeCS4CameraComputeShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            RHICmdList.SetComputeShader(ComputeShader->GetComputeShader());

            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Bilinear>::GetRHI();
            // 将参数传递给ComputeShader
            //这里我们实际上能用到的是UAV,追查到SetTexture函数我们可以发现，对于ComputeShader，第二个参数实际上是没有用的
            ComputeShader->SetParameters(RHICmdList, InputTextureRef,
                OutputTextureRef, TextureUAV,
                SamplerState, MapSamplePanelIDSRV[ID]);

            RHICmdList.TransitionResource(
                EResourceTransitionAccess::ERWNoBarrier,
                EResourceTransitionPipeline::EGfxToCompute,
                TextureUAV);
            DispatchComputeShader(RHICmdList, *ComputeShader, GroupSizeX, GroupSizeY, 1);

            //把CS输出的UAV贴图拷贝到RenderTargetTexture
            RHICmdList.CopyTexture(CreatedRHITexture, OutRenderTargetTexture, FRHICopyTextureInfo());
            //UE_LOG(LogTemp, Log, TEXT("UseComputeShader_RenderThread : Texture Size: %d x %d"), SizeX, SizeY);
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
            //创建贴图资源的SRV视图
            FShaderResourceViewRHIRef MipmapInputSRV = RHICreateShaderResourceView(InputRenderTargetTexture, 0, 1, PF_FloatRGBA);

            uint32 GroupSize = 32;
            uint32 MipSizeX = FMath::DivideAndRoundUp(InputTextureRenderTargetResource->GetSizeX(), uint32(2));
            uint32 MipSizeY = FMath::DivideAndRoundUp(InputTextureRenderTargetResource->GetSizeY(), uint32(2));
            //uint32 MipSizeX = InputTextureRenderTargetResource->GetSizeX() >> 1;
            //uint32 MipSizeY = InputTextureRenderTargetResource->GetSizeY() >> 1;

            //两个整数相除后向上取整
            uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)MipSizeX, GroupSize);
            uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)MipSizeY, GroupSize);

            FRHIResourceCreateInfo MipmapOutputInfo;
            FTexture2DRHIRef MipmapOutputRHITexture = RHICreateTexture2D(MipSizeX, MipSizeY,
                PF_FloatRGBA, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, MipmapOutputInfo);
            //创建贴图资源的UAV视图
            FUnorderedAccessViewRHIRef MipmapOutputUAV = RHICreateUnorderedAccessView(MipmapOutputRHITexture);
            TRefCountPtr<FRHITexture> MipmapOutputTextureRef(MipmapOutputRHITexture);

            //选取FMipmapsComputeShader
            TShaderMapRef<FMipmapsComputeShader> MipmapComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            RHICmdList.SetComputeShader(MipmapComputeShader->GetComputeShader());

            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Bilinear>::GetRHI();
            // 将参数传递给ComputeShader
            //这里我们实际上能用到的是UAV,追查到SetTexture函数我们可以发现，对于ComputeShader，第二个参数实际上是没有用的
            MipmapComputeShader->SetParameters(RHICmdList, MipmapInputSRV, MipmapOutputUAV, SamplerState);

            //TransitionResource 是确保资源正确使用的关键函数，特别是在不同管线（如图形管线和计算管线）之间切换时。
            //它的作用是防止资源冲突并确保 GPU 按照预期顺序访问资源。在 Compute Shader 调用之前进行状态切换是标准流程，以避免访问未同步的资源数据。
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
    static bool bCalGaussKernel = false;
    //存储数据，准备从CPU传递到GPU。
    static TResourceArray<float>* GaussBlur1d = new TResourceArray<float>();
    static float oldBloomStageSize = 0.0;

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
            // UE_LOG(LogTemp, Log, TEXT("SizeX %d, BloomStage.Size * 4.0= %f , BlurRadius %f"),
            //         SizeX, BloomStage.Size * 4.0, BlurRadius);
            GaussBlur1d->Empty();
            Compute1DGaussianFilterKernel(*GaussBlur1d, 32, BlurRadius);
            //CalGaussian1dKernel(*GaussBlur1d, FMath::CeilToInt(BlurRadius), sd);
            if (!direction)
            {
                for (int i = 0; i < (*GaussBlur1d).Num(); i++)
                {
                    // UE_LOG(LogTemp, Log, TEXT("direction %d GaussBlur1d[%d] %f"),
                    //     direction, i,(*GaussBlur1d)[i]);
                }
            }

            if (direction)
            {
                for (int i = 0; i < (*GaussBlur1d).Num(); i++)
                {
                    (*GaussBlur1d)[i] *= (BloomStage.Tint.R);
                    // UE_LOG(LogTemp, Log, TEXT("direction %d GaussBlur1d[%d] %f"),
                    //     direction, i, (*GaussBlur1d)[i]);
                }
            }

            for(int i=0;i< GaussBlur1d->Num();i++)
            {
                //UE_LOG(LogTemp, Log, TEXT("GaussBlur1d[%d] %f"), i, (*GaussBlur1d)[i]);
            }

            //两个整数相除后向上取整
            uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)SizeX, GroupSize);
            uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)SizeY, GroupSize);

            //创建一个贴图资源
            FRHIResourceCreateInfo OutputCreateInfo;
            FTexture2DRHIRef OutputRHITexture = RHICreateTexture2D(SizeX, SizeY,
                PF_FloatRGBA, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, OutputCreateInfo);
            //创建贴图资源的UAV视图
            FUnorderedAccessViewRHIRef TextureUAV = RHICreateUnorderedAccessView(OutputRHITexture);
            TRefCountPtr<FRHITexture> OutputTextureRef(OutputRHITexture);

            //选取FGaussianBlurComputeShader
            TShaderMapRef<FGaussianBlurComputeShader> GaussBlurComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            //在GPU上为数据分配空间，存储从CPU传来的数据。
            FStructuredBufferRHIRef GaussBlur1dBuffer;
            //GPU缓冲区在Shader中的接口，确保数据只读。
            FShaderResourceViewRHIRef GaussBlur1dSRV;
            //在缓冲区创建时作为桥梁，将`GaussBlur1d`中的数据传递到`GaussBlur1dBuffer`。
            FRHIResourceCreateInfo GaussBlur1dCreateInfo;

            int BlurLength = GaussBlur1d->Num();
            GaussBlur1dCreateInfo.ResourceArray = GaussBlur1d;
            //使用`RHICreateStructuredBuffer`创建GPU上的缓冲区`GaussBlur1dBuffer`，并通过`FRHIResourceCreateInfo`完成数据的初始化拷贝。
            GaussBlur1dBuffer = RHICreateStructuredBuffer(sizeof(float), sizeof(float) * BlurLength,
                BUF_Static | BUF_ShaderResource, GaussBlur1dCreateInfo);  //可以测试下加上BUF_FastVRAM | BUF_Transient提升性能
            //使用`RHICreateShaderResourceView`为缓冲区创建只读视图`GaussBlur1dSRV`，绑定到Shader中。
            GaussBlur1dSRV = RHICreateShaderResourceView(GaussBlur1dBuffer);

            RHICmdList.SetComputeShader(GaussBlurComputeShader->GetComputeShader());
            //高斯模糊的时候为了精确采样 , 不用插值
            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Point>::GetRHI();

            // 将参数传递给ComputeShader
            //这里我们实际上能用到的是UAV,追查到SetTexture函数我们可以发现，对于ComputeShader，第二个参数实际上是没有用的
            GaussBlurComputeShader->SetParameters(RHICmdList, GaussBlurInputSRV,
                TextureUAV, GaussBlur1dSRV, BlurLength, direction, SamplerState);

            //TransitionResource 是确保资源正确使用的关键函数，特别是在不同管线（如图形管线和计算管线）之间切换时。
            //它的作用是防止资源冲突并确保 GPU 按照预期顺序访问资源。在 Compute Shader 调用之前进行状态切换是标准流程，以避免访问未同步的资源数据。
            RHICmdList.TransitionResource(
                EResourceTransitionAccess::ERWNoBarrier,
                EResourceTransitionPipeline::EGfxToCompute,
                TextureUAV);
            DispatchComputeShader(RHICmdList, *GaussBlurComputeShader, GroupSizeX, GroupSizeY, 1);

            //把CS输出的UAV贴图拷贝到RenderTargetTexture
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
    //存储数据，准备从CPU传递到GPU。
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
            // UE_LOG(LogTemp, Log, TEXT("SizeX %d, BloomStage.Size * 4.0= %f , BlurRadius %f"),
            //     SizeX, BloomStage.Size * 4.0, BlurRadius);
            GaussBlur1d->Empty();
            Compute1DGaussianFilterKernel(*GaussBlur1d, 32, BlurRadius);
            //CalGaussian1dKernel(*GaussBlur1d, FMath::CeilToInt(BlurRadius), sd);
            if (!direction)
            {
                for (int i = 0; i < (*GaussBlur1d).Num(); i++)
                {
                    // UE_LOG(LogTemp, Log, TEXT("direction %d GaussBlur1d[%d] %f"),
                    //     direction, i, (*GaussBlur1d)[i]);
                }
            }

            if (direction)
            {
                for (int i = 0; i < (*GaussBlur1d).Num(); i++)
                {
                    (*GaussBlur1d)[i] *= (BloomStage.Tint.R);
                    // UE_LOG(LogTemp, Log, TEXT("direction %d GaussBlur1d[%d] %f"),
                    //     direction, i, (*GaussBlur1d)[i]);
                }
            }

            //if (!bCalGaussKernel || BloomStage.Size != oldBloomStageSize)
            //{
            //    float BlurRadius = GetBlurRadius(SizeX, BloomStage.Size * 4.0);
            //    UE_LOG(LogTemp, Log, TEXT("SizeX %d, BloomStage.Size * 4.0= %f , BlurRadius %f"), 
            //        SizeX, BloomStage.Size * 4.0,BlurRadius);
            //    Compute1DGaussianFilterKernel(*GaussBlur1d, 32, BlurRadius);
            //    //CalGaussian1dKernel(*GaussBlur1d, FMath::CeilToInt(BlurRadius), sd);
            //    bCalGaussKernel = true;
            //    oldBloomStageSize = BloomStage.Size;
            //}
            //if(BloomStage.Size == oldBloomStageSize || direction)
            //{
            //    for(int i=0; i< (*GaussBlur1d).Num();i++)
            //    {
            //        (*GaussBlur1d)[i] *= BloomStage.Tint.R;
            //    }
            //}
            // for (int i = 0; i < GaussBlur1d->Num(); i++)
            // {
            //     UE_LOG(LogTemp, Log, TEXT("GaussBlur1d[%d] %f"), i, (*GaussBlur1d)[i]);
            // }

            //两个整数相除后向上取整
            uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)SizeX, GroupSize);
            uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)SizeY, GroupSize);

            //创建一个贴图资源
            FRHIResourceCreateInfo OutputCreateInfo;
            FTexture2DRHIRef OutputRHITexture = RHICreateTexture2D(SizeX, SizeY,
                PF_FloatRGBA, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, OutputCreateInfo);
            //创建贴图资源的UAV视图
            FUnorderedAccessViewRHIRef TextureUAV = RHICreateUnorderedAccessView(OutputRHITexture);
            TRefCountPtr<FRHITexture> OutputTextureRef(OutputRHITexture);

            //选取FGaussianBlurComputeShader
            TShaderMapRef<FGaussianBlurAddComputeShader> GaussBlurAddComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            //在GPU上为数据分配空间，存储从CPU传来的数据。
            FStructuredBufferRHIRef GaussBlur1dBuffer;
            //GPU缓冲区在Shader中的接口，确保数据只读。
            FShaderResourceViewRHIRef GaussBlur1dSRV;
            //在缓冲区创建时作为桥梁，将`GaussBlur1d`中的数据传递到`GaussBlur1dBuffer`。
            FRHIResourceCreateInfo GaussBlur1dCreateInfo;

            int BlurLength = GaussBlur1d->Num();
            GaussBlur1dCreateInfo.ResourceArray = GaussBlur1d;
            //使用`RHICreateStructuredBuffer`创建GPU上的缓冲区`GaussBlur1dBuffer`，并通过`FRHIResourceCreateInfo`完成数据的初始化拷贝。
            GaussBlur1dBuffer = RHICreateStructuredBuffer(sizeof(float), sizeof(float) * BlurLength,
                BUF_Static | BUF_ShaderResource, GaussBlur1dCreateInfo);  //可以测试下加上BUF_FastVRAM | BUF_Transient提升性能
            //使用`RHICreateShaderResourceView`为缓冲区创建只读视图`GaussBlur1dSRV`，绑定到Shader中。
            GaussBlur1dSRV = RHICreateShaderResourceView(GaussBlur1dBuffer);

            RHICmdList.SetComputeShader(GaussBlurAddComputeShader->GetComputeShader());
            //高斯模糊的时候为了精确采样 , 不用插值
            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Point>::GetRHI();
            FSamplerStateRHIRef AddSamplerState = TStaticSamplerState<SF_Bilinear>::GetRHI();

            // 将参数传递给ComputeShader
            //这里我们实际上能用到的是UAV,追查到SetTexture函数我们可以发现，对于ComputeShader，第二个参数实际上是没有用的
            GaussBlurAddComputeShader->SetParameters(RHICmdList, GaussBlurInputSRV, GaussBlurInputAddSRV,
                TextureUAV, GaussBlur1dSRV, BlurLength, direction, SamplerState, AddSamplerState);

            //TransitionResource 是确保资源正确使用的关键函数，特别是在不同管线（如图形管线和计算管线）之间切换时。
            //它的作用是防止资源冲突并确保 GPU 按照预期顺序访问资源。在 Compute Shader 调用之前进行状态切换是标准流程，以避免访问未同步的资源数据。
            RHICmdList.TransitionResource(
                EResourceTransitionAccess::ERWNoBarrier,
                EResourceTransitionPipeline::EGfxToCompute,
                TextureUAV);
            DispatchComputeShader(RHICmdList, *GaussBlurAddComputeShader, GroupSizeX, GroupSizeY, 1);

            //把CS输出的UAV贴图拷贝到RenderTargetTexture
            RHICmdList.CopyTexture(OutputRHITexture, InputRenderTargetTexture, FRHICopyTextureInfo());
        }

    }

}

void UFisheyeCS4CameraRendering::CombineBloom_RenderThread(
    FRHICommandListImmediate& RHICmdList,
    FTextureRenderTargetResource* InputOriTextureRenderTargetResource,
    FTextureRenderTargetResource* InputBlurTextureRenderTargetResource,
    FTextureRenderTargetResource* OutputTextureLDRRenderTargetResource)
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

            //两个整数相除后向上取整
            uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)SizeX, GroupSize);
            uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)SizeY, GroupSize);

            FRHIResourceCreateInfo OutputInfo;
            FTexture2DRHIRef OutputRHITexture = RHICreateTexture2D(SizeX, SizeY,
                PF_B8G8R8A8, 1, 1, TexCreate_ShaderResource | TexCreate_UAV | TexCreate_SRGB, OutputInfo);
            //创建贴图资源的UAV视图
            FUnorderedAccessViewRHIRef OutputUAV = RHICreateUnorderedAccessView(OutputRHITexture);
            TRefCountPtr<FRHITexture> OutputTextureRef(OutputRHITexture);

            //创建贴图资源的SRV视图
            TShaderMapRef<FCombineComputeShader> CombineBloomComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            //选取FMipmapsComputeShader
            RHICmdList.SetComputeShader(CombineBloomComputeShader->GetComputeShader());

            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Bilinear>::GetRHI();

            // 将参数传递给ComputeShader
            //这里我们实际上能用到的是UAV,追查到SetTexture函数我们可以发现，对于ComputeShader，第二个参数实际上是没有用的
            CombineBloomComputeShader->SetParameters(
                RHICmdList, 
                InputOriSRV, 
                InputBlurSRV, 
                InputLutSRV,
                OutputUAV, 
                SamplerState, 
                MapFisheyeMaskSRV[Width]);

            //TransitionResource 是确保资源正确使用的关键函数，特别是在不同管线（如图形管线和计算管线）之间切换时。
            //它的作用是防止资源冲突并确保 GPU 按照预期顺序访问资源。在 Compute Shader 调用之前进行状态切换是标准流程，以避免访问未同步的资源数据。
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
    int ProjectionModel,
    int layout)
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
                SampleNum,
                ProjectionModel,
                layout
            );
            GenMipmap_RenderThread(
                RHICmdList,
                OutTextureRenderTargetResource,
                MipBloomTextureRenderTargetResource[0]);
            //生成Mipmap, i为输入, i+1为输出
            for(int i = 0; i < MipBloomRenderTarget.Num() - 1; i++)
            {
                GenMipmap_RenderThread(
                    RHICmdList,
                    MipBloomTextureRenderTargetResource[i],
                    MipBloomTextureRenderTargetResource[i + 1]);
            }
            //FTexture2DRHIRef LUT = GetSharedLUT(RHICmdList);

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
                OutTextureLDRRenderTargetResource);
        }
        );
        FlushRenderingCommands();
        //UE_LOG(LogTemp, Log, TEXT("ENQUEUE_RENDER_COMMAND"));
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
    *FileWriter << Num;  // 写入元素数量

    for (int32 i = 0; i < Num; ++i)
    {
        int32 Value = Data[i];
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
    //OutData.Empty();
    //OutData.AddUninitialized(Num);

    for (int32 i = 0; i < Num; ++i)
    {
        int32 Value = 0;
        *FileReader << Value;
        OutData[i] = Value;
    }

    FileReader->Close();
    return true;
}

int32 UFisheyeCS4CameraRendering::FindBinFilesInSavedDir(const FString& MatchString, TArray<FString>& OutFoundFiles)
{
    // 获取项目Saved目录
    FString SearchDir = FPaths::ProjectSavedDir();

    // 获取所有文件（递归）
    IFileManager& FileManager = IFileManager::Get();
    TArray<FString> AllFiles;
    FileManager.FindFilesRecursive(AllFiles, *SearchDir, TEXT("*.bin"), true, false);

    // 过滤文件名中包含 MatchString 的
    for (const FString& FilePath : AllFiles)
    {
        FString FileName = FPaths::GetCleanFilename(FilePath);
        if (FileName.Contains(MatchString))
        {
            OutFoundFiles.Add(FilePath);
        }
    }

    return OutFoundFiles.Num(); // 返回找到的文件数量
}


void UFisheyeCS4CameraRendering::TestResourceArraySerialization()
{
    // Step 1: 构造保存路径
    FString FileName = TEXT("FisheyeMask_TestData.bin");
    FString FilePath = FPaths::ProjectSavedDir() / FileName;

    // Step 2: 构造数据
    TResourceArray<int32> OriginalData;
    OriginalData.Add(10);
    OriginalData.Add(20);
    OriginalData.Add(30);
    OriginalData.Add(40);

    // Step 3: 保存
    if (SaveResourceArrayToFile(FilePath, OriginalData))
    {
        UE_LOG(LogTemp, Log, TEXT("Saved file: %s"), *FilePath);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to save file."));
        return;
    }

    // Step 4: 查找含有"FisheyeMask"的bin文件
    TArray<FString> FoundFiles;
    FindBinFilesInSavedDir(TEXT("FisheyeMask"), FoundFiles);

    UE_LOG(LogTemp, Log, TEXT("Found %d matching files."), FoundFiles.Num());
    for (const FString& FoundFile : FoundFiles)
    {
        UE_LOG(LogTemp, Log, TEXT("Matched File: %s"), *FoundFile);
    }

    // Step 5: 尝试从找到的第一个文件读取
    if (FoundFiles.Num() > 0)
    {
        TResourceArray<int32> LoadedData;
        if (LoadResourceArrayFromFile(FoundFiles[0], LoadedData))
        {
            UE_LOG(LogTemp, Log, TEXT("Successfully loaded data from: %s"), *FoundFiles[0]);
            for (int32 i = 0; i < LoadedData.Num(); ++i)
            {
                UE_LOG(LogTemp, Log, TEXT("LoadedData[%d] = %d"), i, LoadedData[i]);
            }
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("Failed to load data from: %s"), *FoundFiles[0]);
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("No matching .bin files found."));
    }
}


void UFisheyeCS4CameraRendering::CalPixelsRelationship(
    FIntPoint Resolution,
    int SampleNum,
    int ProjectionModel,
    int layout)
{
    ID = FString::FromInt(Resolution.X) + TEXT("x") +
        FString::FromInt(Resolution.Y) + TEXT("_") +
        FString::FromInt(ProjectionModel) + TEXT("_") +
        FString::FromInt(layout);
    Width = Resolution.X;

    //如果内存中有ID表和Mask表，直接返回
    if(MapSamplePanelID.Contains(ID) && MapFisheyeMask.Contains(Width))
    {
        UE_LOG(LogTemp, Warning, TEXT("LUT found in RAM! MapSamplePanelID.Contains(ID) && MapFisheyeMask.Contains(Width)"));
        return;
    }
    //没有表，读disk或计算
    else
    {
        TArray<FString> OutFoundIDFiles;
        TArray<FString> OutFoundMaskFiles;

        TSharedPtr<TResourceArray<int>> SamplePanelIDptr = MakeShared<TResourceArray<int>>();
        SamplePanelIDptr->Init(-1, Resolution.X * Resolution.Y * SampleNum * SampleNum + 1);
        MapSamplePanelID.Add(ID, SamplePanelIDptr);

        TSharedPtr<TResourceArray<int>> FisheyeMaskptr = MakeShared<TResourceArray<int>>();
        FisheyeMaskptr->Init(0, Resolution.X * Resolution.Y);
        MapFisheyeMask.Add(Width, FisheyeMaskptr);
        //如果有存储，直接读取后返回
        if(FindBinFilesInSavedDir(TEXT("ID_") + ID + TEXT(".bin"), OutFoundIDFiles) &&
            FindBinFilesInSavedDir(TEXT("Mask_") + FString::FromInt(Width) + TEXT(".bin"), OutFoundMaskFiles))
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
        //内存没有表也没有存储，计算
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("LUT can't found in RAM and Disk! Cal"));
            //O为球心也是3D局部坐标系的原点 , O在原成像面的投影是o , 相距的距离为单位距离1
            //先假设是stereographic投影 , r = 2ftan(θ/2), 暂时f=1
            //这里要注意 , 坐标系是ue4的左手系 , 红色轴x绿色轴y蓝色轴z . 
            //垂直于成像面朝前是x轴 , 成像面水平方向从左到右为y轴, 从下到上为z轴 , 成像面为yz平面
            //设入射光线从点P经过O , 最后落在成像面上的点p(注意大P小p)
            FVector o(-1, 0, 0);
            FVector O(0, 0, 0);
            FVector oO = O - o;
            FVector YNormal(0, 1, 0);

            TArray<FPlane> PlaneArray;
            PlaneArray.Add(FPlane(1, -1, 0, UE_SQRT_2));
            PlaneArray.Add(FPlane(1, 1, 0, UE_SQRT_2));
            PlaneArray.Add(FPlane(0, 0, 1, 1));
            PlaneArray.Add(FPlane(0, 0, 1, -1));

            float SampleDist = 1.0 / (2.0 * float(SampleNum));
            float Radius = FMath::Min(Resolution.X, Resolution.Y) / 2.0;
            (*SamplePanelIDptr)[Resolution.X * Resolution.Y * SampleNum * SampleNum] = layout;
            //从上到下i, 从左到右j
            for (int i = 0; i < Resolution.Y; i++)
            {
                for (int j = 0; j < Resolution.X; j++)
                {
                    //sample point
                    float Samplei;
                    float Samplej;
                    TArray<int> PixelCountPanel;
                    PixelCountPanel.Init(0, 5);
                    //UE_LOG(LogTemp, Warning, TEXT("PixelInfo : (%d,%d)"), i, j);
                    for (int k = 0; k < SampleNum; k++)
                    {
                        for (int l = 0; l < SampleNum; l++)
                        {
                            TArray<int> SampleCountPanel;
                            SampleCountPanel.Init(0, 5);
                            Samplei = float(i) + SampleDist * (2 * k + 1);
                            Samplej = float(j) + SampleDist * (2 * l + 1);

                            if (IsSampleInCircle(Samplei, Samplej, Resolution))
                            {
                                int SampleID = k * SampleNum + l;
                                FVector p(-1, (Samplej - Radius) / Radius, (-Samplei + Radius) / Radius);
                                FVector po = o - p;
                                FVector pO = O - p;
                                //thetad是pO和oO的夹角 , 也就是逆向的出射光线和x轴正向的夹角
                                float thetad = FMath::Acos(FVector::DotProduct(oO, pO) / (oO.Size() * pO.Size()));
                                //theta是OP和oO轴的夹角 , 也就是逆向的入射光线和x轴正向的夹角
                                float theta;
                                switch (ProjectionModel)
                                {
                                case 0:
                                    //透视投影 (perspective projection)
                                    theta = thetad;
                                    break;
                                case 1:
                                    //体视投影 (stereographic projection)
                                    theta = 2 * FMath::Atan(FMath::Tan(thetad) / 2);
                                    break;
                                case 2:
                                    //等距投影 (equidistance projection)
                                    theta = FMath::Tan(thetad);
                                    break;
                                case 3:
                                    //等积投影(equisolid angle projection)
                                    theta = 2 * FMath::Asin(FMath::Tan(thetad) / 2);
                                    break;
                                case 4:
                                    //正交投影 (orthogonal projection)
                                    theta = FMath::Asin(FMath::Tan(thetad));
                                    break;
                                default:
                                    theta = thetad;
                                }

                                //alpha是po和和y轴正向的夹角 , 同样是Op'(p'是P点在yz平面上的投影)和y轴正向的夹角
                                FVector ppie(0, p.Y, p.Z);
                                float alpha = FMath::Acos(FVector::DotProduct(ppie, YNormal) / (ppie.Size() * YNormal.Size()));
                                //上面用反余弦函数求到的角度范围为[0,pi] , 而半球在xy平面的投影(即成像面)的角度是[0,2pi] , 所以需要纠正
                                if (ppie.Z < 0)
                                {
                                    alpha = 2 * PI - alpha;
                                }
                                //现在 , 已知OP和x轴正向角度为theta  , Op'和y轴正向的角度为alpha , 计算出OP的单位向量
                                FVector OPNormal(FMath::Cos(theta), FMath::Sin(theta) * FMath::Cos(alpha), FMath::Sin(theta) * FMath::Sin(alpha));

                                int HitPanelCount = 0;
                                for (int m = 0; m < PlaneArray.Num(); m++)
                                {
                                    //归一化的空间坐标下的交点
                                    FVector IntersectPointNormal = RayPlaneIntersection(FVector::ZeroVector, 0.5 * OPNormal, PlaneArray[m]);
                                    //当找到OP和2D图像的交点
                                    if (IsInRange(IntersectPointNormal))
                                    {
                                        //连续的屏幕坐标 , 坐标原点在左上角 , 竖直朝下是i(x), 水平朝右是j(y)
                                        FVector Intersect = LoclSpace2Panel(m, IntersectPointNormal);
                                        int X = int(Intersect.X * float(Resolution.X) / 2);
                                        int Y = int(Intersect.Y * float(Resolution.Y) / 2);
                                        if (X >= Resolution.Y || Y >= Resolution.X)
                                            break;
                                        //int debugpacked;
                                        int id;
                                        int x;
                                        int y;
                                        int coordx;
                                        int coordy;
                                        int coordindex;
                                        switch ((*SamplePanelIDptr)[Resolution.X * Resolution.Y * SampleNum * SampleNum])
                                        {
                                        case 0:
                                            //0:16x1
                                            coordx = j * 16 + SampleID;
                                            coordy = i;
                                            coordindex = coordy * Resolution.X * 16 + coordx;
                                            break;
                                        case 1:
                                            //1:8x2
                                            coordx = j * 8 + SampleID % 8;
                                            coordy = i * 2 + SampleID / 8;
                                            coordindex = coordy * Resolution.X * 8 + coordx;
                                            break;
                                        case 2:
                                            //2:4x4
                                            coordx = j * 4 + SampleID % 4;
                                            coordy = i * 4 + SampleID / 4;
                                            coordindex = coordy * Resolution.X * 4 + coordx;
                                            break;
                                        case 3:
                                            //3:2x8
                                            coordx = j * 2 + SampleID % 2;
                                            coordy = i * 8 + SampleID / 2;
                                            coordindex = coordy * Resolution.X * 2 + coordx;
                                            break;
                                        case 4:
                                            //4:1x16
                                            coordx = j;
                                            coordy = i * 16 + SampleID;
                                            coordindex = coordy * Resolution.X + coordx;
                                            break;
                                        default:
                                            //as 16x1
                                            coordx = j * 16 + SampleID;
                                            coordy = i;
                                            coordindex = coordy * Resolution.X * 16 + coordx;
                                            break;
                                        }
                                        PackToInt32((*SamplePanelIDptr)[coordindex], m, X, Y);
                                        UnpackFromInt32((*SamplePanelIDptr)[coordindex], id, x, y);
                                        check(id == m && x == X && y == Y);
                                        PixelCountPanel[m]++;
                                        SampleCountPanel[m]++;
                                        HitPanelCount++;
                                        (*FisheyeMaskptr)[i * Resolution.X + j] = 1;
                                        //break;
                                    }
                                }
                                int SampleSampleCountPanelTotal = 0;
                                for (int a = 0; a < SampleCountPanel.Num(); a++)
                                {
                                    SampleSampleCountPanelTotal += SampleCountPanel[a];
                                }
                            }
                        }
                    }
                }
            }

            //存储
            FString IDFileName = TEXT("ID_") + ID + TEXT(".bin");
            FString IDFilePath = FPaths::ProjectSavedDir() / IDFileName;
            FString MaskFileName = TEXT("Mask_") + FString::FromInt(Width) + TEXT(".bin");
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
            //UE_LOG(LogTemp, Log, TEXT("UFisheyeCS4CameraRendering::FisheyeMaskptr num() %d"), FisheyeMaskptr->Num());
        }
        FRHIResourceCreateInfo* CreateInfoPtr = new FRHIResourceCreateInfo();
        CreateInfoPtr->ResourceArray = SamplePanelIDptr.Get();

        // 创建 StructuredBuffer
        FStructuredBufferRHIRef Buffer = RHICreateStructuredBuffer(
            sizeof(int),
            sizeof(int) * SamplePanelIDptr->Num(),
            BUF_Static | BUF_ShaderResource,
            *CreateInfoPtr
        );

        // 创建 ShaderResourceView
        FShaderResourceViewRHIRef SRV = RHICreateShaderResourceView(Buffer);

        // 存入 Map
        MapSamplePanelIDBuffer.Add(ID, Buffer);
        MapSamplePanelIDSRV.Add(ID, SRV);
        MapCreateInfoSamplePanelID.Add(ID, CreateInfoPtr);


        FRHIResourceCreateInfo* FisheyeMaskCreateInfoPtr = new FRHIResourceCreateInfo();
        FisheyeMaskCreateInfoPtr->ResourceArray = FisheyeMaskptr.Get();

        FStructuredBufferRHIRef FisheyeMaskBuffer = RHICreateStructuredBuffer(
            sizeof(int),
            sizeof(int) * FisheyeMaskptr->Num(),
            BUF_Static | BUF_ShaderResource,
            *FisheyeMaskCreateInfoPtr);
        FShaderResourceViewRHIRef FisheyeMaskSRV = RHICreateShaderResourceView(FisheyeMaskBuffer);

        MapFisheyeMaskBuffer.Add(Width, FisheyeMaskBuffer);
        MapFisheyeMaskSRV.Add(Width, FisheyeMaskSRV);
        MapFisheyeMaskCreateInfo.Add(Width, FisheyeMaskCreateInfoPtr);
    }
}

bool UFisheyeCS4CameraRendering::IsSampleInCircle(float i, float j, FIntPoint Resolution)
{
    FVector2D SamplePoint(i, j);

    FVector2D ImageCenter(Resolution.X / 2, Resolution.Y / 2);
    float Dist = (ImageCenter - SamplePoint).Size();
    return Dist <= ImageCenter.X;
}

FVector UFisheyeCS4CameraRendering::RayPlaneIntersection(FVector RayOrigin, FVector RayDirection, FPlane Plane)
{
    FVector PlaneNormal = FVector(Plane.X, Plane.Y, Plane.Z);
    //FVector PlaneOrigin = PlaneNormal * Plane.W;
    FVector PlaneOrigin = GetRandomPointOnPlane(Plane);
    const float Distance = FVector::DotProduct((PlaneOrigin - RayOrigin), PlaneNormal) / FVector::DotProduct(RayDirection, PlaneNormal);
    return RayOrigin + RayDirection * Distance;
}

FVector UFisheyeCS4CameraRendering::LoclSpace2Panel(int PanelID, FVector IntersectPoint)
{
    FMatrix TranslationMatrix;
    FMatrix RotationMatrix;
    FMatrix M;
    switch (PanelID)
    {
        //left
    case 0:
        TranslationMatrix = FTranslationMatrix(FVector(0.0f, -UE_SQRT_2, 1.0f));
        //for(int i=0;i<4;i++)
        //{
        //    UE_LOG(LogTemp, Warning, TEXT("TranslationMatrix:(%lf, %lf, %lf,%lf)"),
        //        TranslationMatrix.M[0][i], TranslationMatrix.M[1][i], TranslationMatrix.M[2][i], TranslationMatrix.M[3][i]);
        //}
        RotationMatrix = FRotationMatrix::MakeFromXY(
            FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.0f),
            FVector(0.0f, 0.0f, -1.0f));
        //for (int i = 0; i < 4; i++)
        //{
        //    UE_LOG(LogTemp, Warning, TEXT("RotationMatrix:(%lf, %lf, %lf,%lf)"),
        //        RotationMatrix.M[0][i], RotationMatrix.M[1][i], RotationMatrix.M[2][i], RotationMatrix.M[3][i]);
        //}
        break;
        //right
    case 1:
        TranslationMatrix = FTranslationMatrix(FVector(UE_SQRT_2, 0.0f, 1.0f));
        RotationMatrix = FRotationMatrix::MakeFromXY(
            FVector(-UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.0f),
            FVector(0, 0.0f, -1.0f));
        break;
        //top, y = 1
    case 2:
        TranslationMatrix = FTranslationMatrix(FVector(-UE_SQRT_2, 0.0f, 1.0f));
        RotationMatrix = FRotationMatrix::MakeFromXY(
            FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.0f),
            FVector(UE_SQRT_2 / 2, -UE_SQRT_2, 0.0f));
        break;
        //    //bottom, z = 1
    case 3:
        TranslationMatrix = FTranslationMatrix(FVector(UE_SQRT_2, 0.0, -1.0f));
        RotationMatrix = FRotationMatrix::MakeFromXY(
            FVector(-UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.0f),
            FVector(-UE_SQRT_2 / 2, -UE_SQRT_2 / 2, 0.0f));
        break;
    }
    M = RotationMatrix * TranslationMatrix;
    //for (int i = 0; i < 4; i++)
    //{
    //    UE_LOG(LogTemp, Warning, TEXT("M:(%lf, %lf, %lf,%lf)"),
    //        M.M[0][i], M.M[1][i], M.M[2][i], M.M[3][i]);
    //}
    M = M.Inverse();
    //for (int i = 0; i < 4; i++)
    //{
    //    UE_LOG(LogTemp, Warning, TEXT("M.inverse:(%lf, %lf, %lf,%lf)"),
    //        M.M[0][i], M.M[1][i], M.M[2][i], M.M[3][i]);
    //}
    FVector4 Res = M.TransformPosition(IntersectPoint);
    return FVector(Res.X, Res.Y, Res.Z);
}

bool UFisheyeCS4CameraRendering::SmallerAndEqual(float A, float B, float eps = EPSINON)
{
    return FMath::IsNearlyEqual(A, B, eps) || (A < B);
}

FVector UFisheyeCS4CameraRendering::GetRandomPointOnPlane(const FPlane& Plane)
{
    FVector PlaneNormal(Plane.X, Plane.Y, Plane.Z);

    // 假设我们设置 x = 0, y = 0 来求解 z
    if (!FMath::IsNearlyZero(PlaneNormal.Z)) {
        float z = Plane.W / PlaneNormal.Z;
        return FVector(0, 0, z); // 此时得到的是 (0, 0, z) 在平面上
    }

    // 其他情况处理
    if (!FMath::IsNearlyZero(PlaneNormal.Y)) {
        float y = Plane.W / PlaneNormal.Y;
        return FVector(0, y, 0); // 平面上任意一点 (0, y, 0)
    }

    float x = Plane.W / PlaneNormal.X;
    return FVector(x, 0, 0); // 平面上任意一点 (x, 0, 0)
}

bool UFisheyeCS4CameraRendering::IsInRange(FVector Point)
{
    float x = Point.X, y = Point.Y, z = Point.Z;
    if (SmallerAndEqual(-1.0f, z) && SmallerAndEqual(z, 1.0f) && SmallerAndEqual(0.0f, x))
    {
        if (SmallerAndEqual(-UE_SQRT_2, y) && SmallerAndEqual(y, 0.0f))
        {
            if (SmallerAndEqual(0.0f, x) && SmallerAndEqual(x, y + UE_SQRT_2))
            {
                return true;
            }
        }
        else if (SmallerAndEqual(0.0f, y) && SmallerAndEqual(y, UE_SQRT_2))
        {
            if (SmallerAndEqual(0.0f, x) && SmallerAndEqual(x, -y + UE_SQRT_2))
            {
                return true;
            }
        }
    }

    return false;
}

void UFisheyeCS4CameraRendering::BeginDestroy()
{
    UE_LOG(LogTemp, Warning, TEXT("MyUObject is being destroyed! ID = %s"), *ID);
    Super::BeginDestroy();
}


#undef LOCTEXT_NAMESPACE
#pragma optimize("", on)