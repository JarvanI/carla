#include "ShadertestPluginRendering.h"
#include <cassert>
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
#define LOCTEXT_NAMESPACE "ShadertestPlugin"


float UShadertestRendering::GetClampedKernelRadius(uint32 SampleCountMax, float KernelRadius)
{
    return FMath::Clamp<float>(KernelRadius, DELTA, SampleCountMax - 1);
}

int UShadertestRendering::GetIntegerKernelRadius(uint32 SampleCountMax, float KernelRadius)
{
    // Smallest radius will be 1.
    return FMath::Min<int32>(FMath::CeilToInt(GetClampedKernelRadius(SampleCountMax, KernelRadius)), SampleCountMax - 1);
}

float UShadertestRendering::NormalDistributionUnscaled(float X, float Sigma)
{
    const float DX = FMath::Abs(X);
    const float Gaussian = FMath::Exp(-16.7f * FMath::Square(DX / Sigma));
    return Gaussian;
}

void UShadertestRendering::Compute1DGaussianFilterKernel(TResourceArray<float>& Gaussian1dKernel, uint32 SampleCountMax, float KernelRadius)
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
void UShadertestRendering::PackToInt32(int &res, int high4, int mid14, int low14) {
    assert(high4 >= 0 && high4 < (1 << 4));    // Ensure high4 fits in 4 bits
    assert(mid14 >= 0 && mid14 < (1 << 14));   // Ensure mid14 fits in 14 bits
    assert(low14 >= 0 && low14 < (1 << 14));   // Ensure low14 fits in 14 bits

    res = (high4 << 28) | (mid14 << 14) | low14;
}

// Unpack three values from a single int32_t
void UShadertestRendering::UnpackFromInt32(int packed, int &high4, int &mid14, int &low14) {
    high4 = (packed >> 28) & 0xF;       // Extract high 4 bits
    mid14 = (packed >> 14) & 0x3FFF;    // Extract middle 14 bits
    low14 = packed & 0x3FFF;            // Extract low 14 bits
}


UShadertestRendering::UShadertestRendering(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
}

class FFisheyecamera5ComputeShader : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FFisheyecamera5ComputeShader, Global)

public:
    FFisheyecamera5ComputeShader() {}
    FFisheyecamera5ComputeShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
        : FGlobalShader(Initializer)
    {
        InputTexture.Bind(Initializer.ParameterMap, TEXT("InputTexture"));
        RWOutputTexture.Bind(Initializer.ParameterMap, TEXT("OutputTexture"));
        RWMipBloomTexture0.Bind(Initializer.ParameterMap, TEXT("MipBloomTexture0"));
        SamplePanelID.Bind(Initializer.ParameterMap, TEXT("SamplePanelID"));
    }

    void SetParameters(
        FRHICommandListImmediate& RHICmdList,
        TArray<TRefCountPtr<FRHITexture>> InputTextureRef,
        FTextureRHIRef& OutTextureRef,
        FUnorderedAccessViewRHIRef& OutputTextureUAVRef,
        FTextureRHIRef& MipBloomTextureRef,
        FUnorderedAccessViewRHIRef& MipBloomTextureUAVRef,
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
        RWMipBloomTexture0.SetTexture(RHICmdList, GetComputeShader(), MipBloomTextureRef, MipBloomTextureUAVRef);
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
        Ar << RWMipBloomTexture0;
        Ar << SamplePanelID;
        return bShaderHasOutdatedParameters;
    }

private:
    FShaderResourceParameter InputTexture;
    FRWShaderParameter RWOutputTexture;
    FRWShaderParameter RWMipBloomTexture0;
    FShaderResourceParameter SamplePanelID;
};
IMPLEMENT_SHADER_TYPE(, FFisheyecamera5ComputeShader, TEXT("/Plugin/ShadertestPlugin/Private/TexturePacker.usf"), TEXT("MainCS"), SF_Compute)


class FMipmapComputeShader : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FMipmapComputeShader, Global)

public:
    FMipmapComputeShader() {}
    FMipmapComputeShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
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
IMPLEMENT_SHADER_TYPE(, FMipmapComputeShader, TEXT("/Plugin/ShadertestPlugin/Private/GenMipmap.usf"), TEXT("MipmapCS"), SF_Compute)

class FGaussBlurComputeShader : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FGaussBlurComputeShader, Global)

public:
    FGaussBlurComputeShader() {}
    FGaussBlurComputeShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
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
IMPLEMENT_SHADER_TYPE(, FGaussBlurComputeShader, TEXT("/Plugin/ShadertestPlugin/Private/GaussBlur1d.usf"), TEXT("GaussBlur1dCS"), SF_Compute)

class FGaussBlurAddComputeShader : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FGaussBlurAddComputeShader, Global)
public:
    FGaussBlurAddComputeShader() {}
    FGaussBlurAddComputeShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
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
IMPLEMENT_SHADER_TYPE(, FGaussBlurAddComputeShader, TEXT("/Plugin/ShadertestPlugin/Private/GaussBlur1dAdd.usf"), TEXT("GaussBlur1dAddCS"), SF_Compute)

class FUpscalingComputeShader : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FUpscalingComputeShader, Global)

public:
    FUpscalingComputeShader() {}
    FUpscalingComputeShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
        : FGlobalShader(Initializer)
    {
        // 绑定输入纹理（只读 SRV）
        InputHighResOriTexture.Bind(Initializer.ParameterMap, TEXT("InputHighResOriTexture"));
        InputLowResBlurTexture.Bind(Initializer.ParameterMap, TEXT("InputLowResBlurTexture"));
        // 绑定输出纹理（可写 UAV）
        RWOutputUpscaledTexture.Bind(Initializer.ParameterMap, TEXT("RWOutputUpscaledTexture"));
        UpscaledSampler.Bind(Initializer.ParameterMap, TEXT("UpscaledSampler"));
    }

    // 设置着色器参数（输入 SRV 和输出 UAV）
    void SetParameters(
        FRHICommandListImmediate& RHICmdList,
        FShaderResourceViewRHIRef& InputHighResOri,
        FShaderResourceViewRHIRef& InputLowResBlur,
        FUnorderedAccessViewRHIRef& OutputUpscaled,
        FSamplerStateRHIRef& SamplerState)
    {
        // 设置输入纹理的 SRV
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), InputHighResOriTexture.GetBaseIndex(), InputHighResOri);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), InputLowResBlurTexture.GetBaseIndex(), InputLowResBlur);
        // 设置输出纹理的 UAV
        RHICmdList.SetUAVParameter(GetComputeShader(), RWOutputUpscaledTexture.GetUAVIndex(), OutputUpscaled);
        RHICmdList.SetShaderSampler(GetComputeShader(), UpscaledSampler.GetBaseIndex(), SamplerState);
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
        Ar << InputHighResOriTexture;
        Ar << InputLowResBlurTexture;
        Ar << RWOutputUpscaledTexture;
        Ar << UpscaledSampler;
        return bShaderHasOutdatedParameters;
    }

private:
    // 输入高分辨率原图纹理（只读 SRV）
    FShaderResourceParameter InputHighResOriTexture;
    // 输入低分辨率模糊图纹理（只读 SRV）:
    FShaderResourceParameter InputLowResBlurTexture;
    // 输出纹理（可写 UAV）
    FRWShaderParameter RWOutputUpscaledTexture;
    // 采样器
    FShaderResourceParameter UpscaledSampler;
};
IMPLEMENT_SHADER_TYPE(, FUpscalingComputeShader, TEXT("/Plugin/ShadertestPlugin/Private/Upscaling.usf"), TEXT("UpscalingCS"), SF_Compute)

class FCombineBloomComputeShader : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FCombineBloomComputeShader, Global)

public:
    FCombineBloomComputeShader() {}
    FCombineBloomComputeShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
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
IMPLEMENT_SHADER_TYPE(, FCombineBloomComputeShader, TEXT("/Plugin/ShadertestPlugin/Private/CombineBloom.usf"), TEXT("CombineBloomCS"), SF_Compute)


class FLUTComputeShader : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FLUTComputeShader, Global)

public:
    FLUTComputeShader() {}
    FLUTComputeShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
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
IMPLEMENT_SHADER_TYPE(, FLUTComputeShader, TEXT("/Plugin/ShadertestPlugin/Private/LUT.usf"), TEXT("LUTCS"), SF_Compute)


FTexture2DRHIRef UShadertestRendering::GetSharedLUT(FRHICommandListImmediate& RHICmdList) {
    static FTexture2DRHIRef Texture = CreateLUT(RHICmdList);
    //FTexture2DRHIRef Texture = CreateLUT(RHICmdList);
    return Texture;
}

FTexture2DRHIRef UShadertestRendering::CreateLUT(FRHICommandListImmediate& RHICmdList)
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
    TShaderMapRef<FLUTComputeShader> LUTComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    //选取FMipmapComputeShader
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

FTexture3DRHIRef UShadertestRendering::CreateLUT3D(FRHICommandListImmediate& RHICmdList)
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
    TShaderMapRef<FLUTComputeShader> LUTComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    //选取FMipmapComputeShader
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


void UShadertestRendering::UseComputeShaderArray_RenderThread(
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

    if (OutTextureRenderTargetResource)
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

            FRHIResourceCreateInfo MipmapOutputInfo;
            FTexture2DRHIRef MipmapOutputRHITexture = RHICreateTexture2D(SizeX, SizeY,
                PF_FloatRGBA, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, MipmapOutputInfo);
            //创建贴图资源的UAV视图
            FUnorderedAccessViewRHIRef MipmapOutputUAV = RHICreateUnorderedAccessView(MipmapOutputRHITexture);
            TRefCountPtr<FRHITexture> MipmapOutputTextureRef(MipmapOutputRHITexture);
            //选取FMipmapComputeShader
            TShaderMapRef<FMipmapComputeShader> MipmapComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            TArray<TRefCountPtr<FRHITexture>> InputTextureRef;
            for (int i = 0; i < InRenderTargetTexture.Num(); i++)
            {
                InputTextureRef.Add(TRefCountPtr<FRHITexture>(InRenderTargetTexture[i]));
            }

            static uint32 Count = 0;
            static TShaderMapRef<FFisheyecamera5ComputeShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            //存储数据，准备从CPU传递到GPU。
            static TResourceArray<int>* SamplePanelID = new TResourceArray<int>();
            //在GPU上为数据分配空间，存储从CPU传来的数据。
            static FStructuredBufferRHIRef SamplePanelIDBuffer;
            //GPU缓冲区在Shader中的接口，确保数据只读。
            static FShaderResourceViewRHIRef SamplePanelIDSRV;
            //在缓冲区创建时作为桥梁，将`SamplePanelID`中的数据传递到`SamplePanelIDBuffer`。
            static FRHIResourceCreateInfo CreateInfoSamplePanelID;

            static int OldProjectionModel = -1;
            static int OldLayout = -1;
            if (Count == 0)
            {
                UE_LOG(LogTemp, Warning, TEXT("if(Count == 0)"));
                SamplePanelID->Init(-1, SizeX * SizeY * SampleNum * SampleNum + 1);
                PixelInCircle.Empty();
                PixelInCircle.Init(0, SizeX * SizeY);
                UE_LOG(LogTemp, Warning, TEXT("before SizeX %d ,SizeY %d, SampleNum %d, SamplePanelID %d"),
                    SizeX, SizeY, SampleNum, SamplePanelID->Num());
                CalPixelsRelationship(*SamplePanelID, Resolution, SampleNum, ProjectionModel, layout);
                UE_LOG(LogTemp, Warning, TEXT("after SizeX %d ,SizeY %d, SampleNum %d, SamplePanelID %d"),
                    SizeX, SizeY, SampleNum, SamplePanelID->Num());

                CreateInfoSamplePanelID.ResourceArray = SamplePanelID;
                //使用`RHICreateStructuredBuffer`创建GPU上的缓冲区`SamplePanelIDBuffer`，并通过`FRHIResourceCreateInfo`完成数据的初始化拷贝。
                SamplePanelIDBuffer = RHICreateStructuredBuffer(sizeof(int), sizeof(int) * SamplePanelID->Num(),
                    BUF_Static | BUF_ShaderResource, CreateInfoSamplePanelID);  //可以测试下加上BUF_FastVRAM | BUF_Transient提升性能
                //使用`RHICreateShaderResourceView`为缓冲区创建只读视图`SamplePanelIDSRV`，绑定到Shader中。
                SamplePanelIDSRV = RHICreateShaderResourceView(SamplePanelIDBuffer);

                OldProjectionModel = ProjectionModel;
                OldLayout = layout;
            }
            Count++;

            //UE_LOG(LogTemp, Warning, TEXT("after after SizeX %d ,SizeY %d, SampleNum %d, SamplePanelID %d"),
            //    SizeX, SizeY, SampleNum, SamplePanelID->Num());
            if(OldProjectionModel != ProjectionModel || OldLayout != layout)
            {
                UE_LOG(LogTemp, Warning, TEXT("if(OldProjectionModel != ProjectionModel || OldLayout != layout)"));
                SamplePanelID->Init(-1, SizeX * SizeY * SampleNum * SampleNum + 1);
                CalPixelsRelationship(*SamplePanelID, Resolution, SampleNum, ProjectionModel, layout);

                CreateInfoSamplePanelID.ResourceArray = SamplePanelID;
                SamplePanelIDBuffer = RHICreateStructuredBuffer(sizeof(int), sizeof(int) * SamplePanelID->Num(),
                    BUF_Static | BUF_ShaderResource, CreateInfoSamplePanelID);
                SamplePanelIDSRV = RHICreateShaderResourceView(SamplePanelIDBuffer);

                OldProjectionModel = ProjectionModel;
                OldLayout = layout;
            }
            RHICmdList.SetComputeShader(ComputeShader->GetComputeShader());

            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Bilinear>::GetRHI();
            // 将参数传递给ComputeShader
            //这里我们实际上能用到的是UAV,追查到SetTexture函数我们可以发现，对于ComputeShader，第二个参数实际上是没有用的
            ComputeShader->SetParameters(RHICmdList, InputTextureRef,
                OutputTextureRef, TextureUAV, 
                MipmapOutputTextureRef, MipmapOutputUAV,
                SamplerState, SamplePanelIDSRV);

            //TransitionResource 是确保资源正确使用的关键函数，特别是在不同管线（如图形管线和计算管线）之间切换时。
            //它的作用是防止资源冲突并确保 GPU 按照预期顺序访问资源。在 Compute Shader 调用之前进行状态切换是标准流程，以避免访问未同步的资源数据。
            RHICmdList.TransitionResource(
                EResourceTransitionAccess::ERWNoBarrier,
                EResourceTransitionPipeline::EGfxToCompute,
                TextureUAV);
            DispatchComputeShader(RHICmdList, *ComputeShader, GroupSizeX, GroupSizeY, 1);

            //把CS输出的UAV贴图拷贝到RenderTargetTexture
            RHICmdList.CopyTexture(CreatedRHITexture, OutRenderTargetTexture, FRHICopyTextureInfo());
            //RHICmdList.CopyTexture(MipmapOutputRHITexture, MipBloomRenderTargetTexture, FRHICopyTextureInfo());
            UE_LOG(LogTemp, Log, TEXT("UseComputeShader_RenderThread : Texture Size: %d x %d"), SizeX, SizeY);
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


void UShadertestRendering::GenMipmap_RenderThread(
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

            //选取FMipmapComputeShader
            TShaderMapRef<FMipmapComputeShader> MipmapComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
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

float UShadertestRendering::GetBlurRadius(uint32 ViewSize, float KernelSizePercent)
{
    const float PercentToScale = 0.01f;

    const float DiameterToRadius = 0.5f;

    return static_cast<float>(ViewSize) * KernelSizePercent * PercentToScale * DiameterToRadius;
}

void UShadertestRendering::GaussianBlur(
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

            //选取FGaussBlurComputeShader
            TShaderMapRef<FGaussBlurComputeShader> GaussBlurComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

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

void UShadertestRendering::GaussianBlurAdd(
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

            //选取FGaussBlurComputeShader
            TShaderMapRef<FGaussBlurAddComputeShader> GaussBlurAddComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

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


void UShadertestRendering::Upscaling_RenderThread(
    FRHICommandListImmediate& RHICmdList,
    FTextureRenderTargetResource* InputHiResOriTextureRenderTargetResource,
    FTextureRenderTargetResource* InputLowBlurTextureRenderTargetResource)
{
    check(IsInRenderingThread());

    if (InputHiResOriTextureRenderTargetResource && InputLowBlurTextureRenderTargetResource)
    {
        FTexture2DRHIRef InputHiResOriRenderTargetTexture = InputHiResOriTextureRenderTargetResource->GetRenderTargetTexture();
        FTexture2DRHIRef InputLowBlurRenderTargetTexture = InputLowBlurTextureRenderTargetResource->GetRenderTargetTexture();
        if (InputHiResOriRenderTargetTexture.IsValid() && InputLowBlurRenderTargetTexture.IsValid())
        {
            FShaderResourceViewRHIRef InputHiResOriSRV = RHICreateShaderResourceView(InputHiResOriRenderTargetTexture, 0, 1, PF_FloatRGBA);
            FShaderResourceViewRHIRef InputLowBlurSRV = RHICreateShaderResourceView(InputLowBlurRenderTargetTexture, 0, 1, PF_FloatRGBA);

            uint32 GroupSize = 32;
            uint32 SizeX = InputHiResOriRenderTargetTexture->GetSizeX();
            uint32 SizeY = InputHiResOriRenderTargetTexture->GetSizeY();

            //两个整数相除后向上取整
            uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)SizeX, GroupSize);
            uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)SizeY, GroupSize);

            FRHIResourceCreateInfo OutputInfo;
            FTexture2DRHIRef OutputRHITexture = RHICreateTexture2D(SizeX, SizeY,
                PF_FloatRGBA, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, OutputInfo);
            //创建贴图资源的UAV视图
            FUnorderedAccessViewRHIRef OutputUAV = RHICreateUnorderedAccessView(OutputRHITexture);
            TRefCountPtr<FRHITexture> OutputTextureRef(OutputRHITexture);
            //创建贴图资源的SRV视图
            TShaderMapRef<FUpscalingComputeShader> UpscalingComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            //选取FMipmapComputeShader
            RHICmdList.SetComputeShader(UpscalingComputeShader->GetComputeShader());

            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Bilinear>::GetRHI();

            // 将参数传递给ComputeShader
            //这里我们实际上能用到的是UAV,追查到SetTexture函数我们可以发现，对于ComputeShader，第二个参数实际上是没有用的
            UpscalingComputeShader->SetParameters(RHICmdList, InputHiResOriSRV, InputLowBlurSRV, OutputUAV,SamplerState);

            //TransitionResource 是确保资源正确使用的关键函数，特别是在不同管线（如图形管线和计算管线）之间切换时。
            //它的作用是防止资源冲突并确保 GPU 按照预期顺序访问资源。在 Compute Shader 调用之前进行状态切换是标准流程，以避免访问未同步的资源数据。
            RHICmdList.TransitionResource(
                EResourceTransitionAccess::ERWNoBarrier,
                EResourceTransitionPipeline::EGfxToCompute,
                OutputUAV);

            DispatchComputeShader(RHICmdList, *UpscalingComputeShader, GroupSizeX, GroupSizeY, 1);
            RHICmdList.CopyTexture(OutputRHITexture, InputHiResOriRenderTargetTexture, FRHICopyTextureInfo());
        }else
        {
            UE_LOG(LogTemp, Error, TEXT("Upscaling_RenderThread : InputHiResOriRenderTargetTexture.IsValid() && InputLowBlurRenderTargetTexture.IsValid() not valid."));
        }
    }else
    {
        UE_LOG(LogTemp, Error, TEXT("Upscaling_RenderThread : InputHiResOriTextureRenderTargetResource && InputLowBlurTextureRenderTargetResource not valid."));
    }
}

void UShadertestRendering::CombineBloom_RenderThread(
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
            if (!(SizeX * SizeY))
            {
                return;
            }

            //两个整数相除后向上取整
            uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)SizeX, GroupSize);
            uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)SizeY, GroupSize);

            FRHIResourceCreateInfo OutputInfo;
            FTexture2DRHIRef OutputRHITexture = RHICreateTexture2D(SizeX, SizeY,
                PF_B8G8R8A8, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, OutputInfo);
            //创建贴图资源的UAV视图
            FUnorderedAccessViewRHIRef OutputUAV = RHICreateUnorderedAccessView(OutputRHITexture);
            TRefCountPtr<FRHITexture> OutputTextureRef(OutputRHITexture);

            static int count = 0;
            //在GPU上为数据分配空间，存储从CPU传来的数据。
            static FStructuredBufferRHIRef FisheyeMaskBuffer;
            //GPU缓冲区在Shader中的接口，确保数据只读。
            static FShaderResourceViewRHIRef FisheyeMaskSRV;
            //在缓冲区创建时作为桥梁，将`FisheyeMask`中的数据传递到`FisheyeMaskBuffer`。
            static FRHIResourceCreateInfo FisheyeMaskCreateInfo;

            if(!count)
            {
                FisheyeMaskCreateInfo.ResourceArray = &PixelInCircle;
                //使用`RHICreateStructuredBuffer`创建GPU上的缓冲区`FisheyeMaskBuffer`，并通过`FRHIResourceCreateInfo`完成数据的初始化拷贝。
                FisheyeMaskBuffer = RHICreateStructuredBuffer(sizeof(int), sizeof(int) * SizeX * SizeY,
                    BUF_Static | BUF_ShaderResource, FisheyeMaskCreateInfo);  //可以测试下加上BUF_FastVRAM | BUF_Transient提升性能
                //使用`RHICreateShaderResourceView`为缓冲区创建只读视图`FisheyeMaskSRV`，绑定到Shader中。
                FisheyeMaskSRV = RHICreateShaderResourceView(FisheyeMaskBuffer);
            }
            count++;


            //创建贴图资源的SRV视图
            TShaderMapRef<FCombineBloomComputeShader> CombineBloomComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            //选取FMipmapComputeShader
            RHICmdList.SetComputeShader(CombineBloomComputeShader->GetComputeShader());

            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Bilinear>::GetRHI();

            // 将参数传递给ComputeShader
            //这里我们实际上能用到的是UAV,追查到SetTexture函数我们可以发现，对于ComputeShader，第二个参数实际上是没有用的
            CombineBloomComputeShader->SetParameters(RHICmdList, InputOriSRV, InputBlurSRV, InputLutSRV,OutputUAV, SamplerState, FisheyeMaskSRV);

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


void UShadertestRendering::CalGaussian1dKernel(TResourceArray<float>& Gaussian1dKernel, int BlurRadius, float sd)
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

void UShadertestRendering::UseComputeShaderArray(
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
        ENQUEUE_RENDER_COMMAND(FisheyeCSCamera)
            (
                [&](FRHICommandListImmediate& RHICmdList)
        {
            //生成fisheye图像
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


void UShadertestRendering::CalPixelsRelationship(
    TResourceArray<int>& SamplePanelID,
    FIntPoint Resolution,
    int SampleNum,
    int ProjectionModel,
    int layout)
{
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
    PlaneArray.Add(FPlane(1, 0, 0, 1));
    PlaneArray.Add(FPlane(0, 1, 0, -1));
    PlaneArray.Add(FPlane(0, 1, 0, 1));
    PlaneArray.Add(FPlane(0, 0, 1, 1));
    PlaneArray.Add(FPlane(0, 0, 1, -1));

    float SampleDist = 1.0 / (2.0 * float(SampleNum));
    float Radius = FMath::Min(Resolution.X, Resolution.Y) / 2.0;
    SamplePanelID[Resolution.X * Resolution.Y * SampleNum * SampleNum] = layout;
    // UE_LOG(LogTemp, Log, TEXT("UShadertestRendering::SamplePanelID[Resolution.X * Resolution.Y * SampleNum * SampleNum] = %d"),
    //     SamplePanelID[Resolution.X * Resolution.Y * SampleNum * SampleNum]);
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
                            bool WillIntersect = false;
                            FVector IntersectPointNormal = RayPlaneIntersection(FVector::ZeroVector, OPNormal, PlaneArray[m], WillIntersect);
                            //当找到OP和2D图像的交点
                            if (WillIntersect && IsPointInCube(IntersectPointNormal))
                            {
                                //局部空间坐标
                                FVector IntersectPoint = IntersectPointNormal * Radius;
                                //连续的屏幕坐标 , 坐标原点在左上角 , 竖直朝下是i(x), 水平朝右是j(y)
                                FVector2D IncidentRayOrigin = LoclSpace2Panel(m, IntersectPoint, Radius);
                                int X = IncidentRayOrigin.Y;
                                int Y = IncidentRayOrigin.X;
                                if (X >= Resolution.Y || Y >= Resolution.X)
                                    break;
                                //int debugpacked;
                                int id;
                                int x;
                                int y;
                                int coordx;
                                int coordy;
                                int coordindex;
                                switch(SamplePanelID[Resolution.X * Resolution.Y * SampleNum * SampleNum])
                                {
                                case 0:
                                    //0:16x1
                                    coordx = j * 16  + SampleID;
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
                                    coordindex = coordy * Resolution.X  + coordx;
                                    break;
                                default:
                                    //as 16x1
                                    coordx = j * 16 + SampleID;
                                    coordy = i;
                                    coordindex = coordy * Resolution.X * 16 + coordx;
                                    break;
                                }
                                PackToInt32(SamplePanelID[coordindex], m, X, Y);
                                UnpackFromInt32(SamplePanelID[coordindex], id, x, y);
                                check(id == m && x == X && y == Y);
                                PixelCountPanel[m]++;
                                SampleCountPanel[m]++;
                                HitPanelCount++;
                                PixelInCircle[i * Resolution.X + j] = 1;
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
}

bool UShadertestRendering::IsSampleInCircle(float i, float j, FIntPoint Resolution)
{
    FVector2D SamplePoint(i, j);

    FVector2D ImageCenter(Resolution.X / 2, Resolution.Y / 2);
    float Dist = (ImageCenter - SamplePoint).Size();
    return Dist <= ImageCenter.X;
}

FVector UShadertestRendering::RayPlaneIntersection(const FVector& RayOrigin, const FVector& RayDirection, const FPlane& Plane, bool& WillIntersection)
{
    const FVector PlaneNormal = FVector(Plane.X, Plane.Y, Plane.Z);
    //w也可以是法线与平面交点到原点距离
    const FVector PlaneOrigin = PlaneNormal * Plane.W;
    const float Distance = FVector::DotProduct((PlaneOrigin - RayOrigin), PlaneNormal) / FVector::DotProduct(RayDirection, PlaneNormal);
    if (Distance >= 0.0)
    {
        WillIntersection = true;
    }
    else
    {
        WillIntersection = false;
    }

    return RayOrigin + RayDirection * Distance;
}

bool UShadertestRendering::IsPointInCube(FVector Point)
{
    if ((FMath::Abs(Point.X) >= 0.0) && (Point.X <= 1.0)
        && (Point.Y >= -1.0) && (Point.Y <= 1.0)
        && (Point.Z >= -1.0) && (Point.Z <= 1.0))
    {
        return true;
    }
    return false;
}

FVector2D UShadertestRendering::LoclSpace2Panel(int PanelID, FVector IntersectPoint, float Radius)
{
    FVector2D IncidentRayOrigin;
    switch (PanelID)
    {
        //front , x = 1
    case 0:
        IncidentRayOrigin = FVector2D(-IntersectPoint.Z + Radius, IntersectPoint.Y + Radius);
        break;
        //left, y = -1
    case 1:
        IncidentRayOrigin = FVector2D(-IntersectPoint.Z + Radius, IntersectPoint.X + Radius);
        break;
        //right, y = 1
    case 2:
        IncidentRayOrigin = FVector2D(-IntersectPoint.Z + Radius, -IntersectPoint.X + Radius);
        break;
        //top, z = 1
    case 3:
        IncidentRayOrigin = FVector2D(IntersectPoint.X + Radius, IntersectPoint.Y + Radius);
        break;
        //bottom, z = -1
    case 4:
        IncidentRayOrigin = FVector2D(-IntersectPoint.X + Radius, IntersectPoint.Y + Radius);
        break;
    }
    return IncidentRayOrigin;
}


#undef LOCTEXT_NAMESPACE
#pragma optimize("", on)