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
    int ProjectionModel)
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
            if(SnitchNum == 4)
            {
                FTexture2DRHIRef DummyTexture = GBlackTexture->TextureRHI->GetTexture2D(); // 黑色占位纹理
                InputTextureRef.Add(TRefCountPtr<FRHITexture>(DummyTexture));
            }

            static uint32 Count = 0;
            static TShaderMapRef<FFisheyeCS4CameraComputeShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            RHICmdList.SetComputeShader(ComputeShader->GetComputeShader());

            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Bilinear>::GetRHI();
            // 将参数传递给ComputeShader
            //这里我们实际上能用到的是UAV,追查到SetTexture函数我们可以发现，对于ComputeShader，第二个参数实际上是没有用的
            ComputeShader->SetParameters(RHICmdList, InputTextureRef,
                OutputTextureRef, TextureUAV,SamplerState, 
                MapSamplePanelIDSRV[ID], SnitchNum);

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
    int ProjectionModel)
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
                ProjectionModel
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
    if (Len <= KINDA_SMALL_NUMBER)
    {
        return FPlane(0, 0, 0, 0); // 防止除以0
    }
    FVector Normalized = N / Len;
    return FPlane(Normalized, P.W / Len); // W 也要除以 Len
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
    if (LengthSq < KINDA_SMALL_NUMBER)
        return 0.0f;

    float T = FVector::DotProduct(AB, AP) / LengthSq;
    return T;
}

bool UFisheyeCS4CameraRendering::IsPointOnPlane(const FVector& Point, const FPlane& Plane, float Tolerance = KINDA_SMALL_NUMBER)
{
    return FMath::Abs(Plane.PlaneDot(Point)) <= Tolerance;
}

bool UFisheyeCS4CameraRendering::IsPointOnEdge(FVector Point)
{
    return (FMath::IsNearlyZero(Point.X, KINDA_SMALL_NUMBER) ||
        FMath::IsNearlyZero(Point.Y, KINDA_SMALL_NUMBER) ||
        FMath::IsNearlyEqual(Point.X, 1.0f * float(Width), KINDA_SMALL_NUMBER) ||
        FMath::IsNearlyEqual(Point.Y, 1.0f * float(Width), KINDA_SMALL_NUMBER));
}

bool UFisheyeCS4CameraRendering::IsOnSameEdge(FVector P1, FVector P2)
{
    return ((FMath::IsNearlyZero(P1.X, KINDA_SMALL_NUMBER) && FMath::IsNearlyZero(P2.X, KINDA_SMALL_NUMBER)) ||
        (FMath::IsNearlyZero(P1.Y, KINDA_SMALL_NUMBER) && FMath::IsNearlyZero(P2.Y, KINDA_SMALL_NUMBER)) ||
        (FMath::IsNearlyEqual(P1.X, 1.0f * float(Width), KINDA_SMALL_NUMBER) && 
            FMath::IsNearlyEqual(P2.X, 1.0f * float(Width), KINDA_SMALL_NUMBER)) ||
        (FMath::IsNearlyEqual(P1.Y, 1.0f * float(Width), KINDA_SMALL_NUMBER) && 
            FMath::IsNearlyEqual(P2.Y, 1.0f * float(Width), KINDA_SMALL_NUMBER)));
}

bool UFisheyeCS4CameraRendering::AddIfCantFind(TArray<FVector>& Group, FVector Point)
{
    bool Found = false;
    for (FVector P : Group)
    {
        if (P.Equals(Point, KINDA_SMALL_NUMBER))
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

void UFisheyeCS4CameraRendering::SplitPoints(TArray<FPointInfo>& InputPoints, TArray<TArray<FVector>>& OutGroups)
{
    int CountBefore = InputPoints.Num();
    bool bAddNewPoint = false;

    //生成面之间的边上的分割点
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

        //两个点不共面 , 就要计算边界点
        if (SharedFaceCount == 0)
        {
            FPlane splitPlane(seg.PStart.WorldPos, seg.PEnd.WorldPos, FVector::ZeroVector);
            TArray<FPointInfo> InsertedPoints;
            TArray<float> TList;

            for (int FaceA = 0; FaceA < PlaneArray.Num() - 1; FaceA++)
            {
                for (int FaceB = FaceA + 1; FaceB < PlaneArray.Num(); FaceB++)
                {
                    // ✅ 排除无效索引（即只允许 AB 各自的面组合）
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
                        if (FVector::DistSquared(Existing.WorldPos, pt) < KINDA_SMALL_NUMBER)
                        {
                            bAlreadyInserted = true;
                            break;
                        }
                    }
                    if (bAlreadyInserted || !IsInRange(pt))
                        continue;

                    float t = GetSegmentTProjection(seg.PStart.WorldPos, seg.PEnd.WorldPos, pt);

                    if (t < KINDA_SMALL_NUMBER || t > 1.0f - KINDA_SMALL_NUMBER)
                        continue;

                    //if (FMath::IsNearlyEqual(t, 0.0f, KINDA_SMALL_NUMBER)) t = 0.0f;
                    //else if (FMath::IsNearlyEqual(t, 1.0f, KINDA_SMALL_NUMBER)) t = 1.0f;

                    // ✅ 提取落在哪些面上
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

                    // ✅ 插入排序按 t 值升序插入
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


            // 插入交点到 InputPoints（从后往前插入防止索引错乱）
            for (int q = InsertedPoints.Num() - 1; q >= 0; q--)
            {
                InputPoints.Insert(InsertedPoints[q], i + 1);
            }

            i += InsertedPoints.Num(); // 跳过新插入的交点
            CountBefore += InsertedPoints.Num();
        }
    }

    //在二维坐标下分点 . 这里的输出是每个面上的二维坐标, FVector.z为0
    {
        for (FPointInfo Point : InputPoints)
        {
            for (int faceidx : Point.FaceIndex)
            {
                OutGroups[faceidx].Add(LocalSpace2Panel(faceidx, Point.WorldPos));
            }
        }

        //如何判断是否要加入三面顶点 ? 就看起点和终点所在边
        if (bAddNewPoint)
        {
            FVector LeftTop = FVector(0.0f, 0.0f, 0.0f);
            FVector RightTop = FVector(1.0f, 0.0f, 0.0f) * float(Width);
            FVector LeftBottom = FVector(0.0f, 1.0f, 0.0f) * float(Width);
            FVector RightBottom = FVector(1.0f, 1.0f, 0.0f) * float(Width);
            //先找到起点和终点 , 也就是入面点和出面点
            for (int i = 0; i < OutGroups.Num(); i++)
            {
                TArray<FVector>& Group = OutGroups[i];
                if (Group.Num() > 1)
                {
                    FVector InFace = FVector(-1.0f, -1.0f, -1.0f);
                    FVector OutFace = FVector(-1.0f, -1.0f, -1.0f);
                    for (int j = 0; j < Group.Num(); j++)
                    {
                        FVector Start = Group[j];
                        FVector End = Group[(j + 1) % Group.Num()];
                        bool IsStartOnEdge = IsPointOnEdge(Start);
                        bool IsEndOnEdge = IsPointOnEdge(End);
                        if (IsStartOnEdge && !IsEndOnEdge)
                        {
                            InFace = Start;
                        }
                        else if (!IsStartOnEdge && IsEndOnEdge)
                        {
                            OutFace = End;
                        }
                        else if (IsStartOnEdge && IsEndOnEdge && !IsOnSameEdge(Start, End) && Group.Num() == 2)
                        {
                            InFace = Start;
                            OutFace = End;
                        }
                    }
                    if(InFace.Equals(FVector(-1.0f, -1.0f, -1.0f)))
                    {
                        UE_LOG(LogTemp, Warning, TEXT("Pixel InFace.Equals(FVector(-1.0f, -1.0f, -1.0f))"));
                        for (int g = 0; g < OutGroups.Num(); g++)
                        {
                            TArray<FVector>& Grouptest = OutGroups[i];
                            for (int j = 0; j < Grouptest.Num(); j++)
                            {
                                FVector newp = Group[j];
                                UE_LOG(LogTemp, Warning, TEXT("Group[%d] : Point[%d] (%.4f, %.4f, %.4f)"), g, j, newp.X, newp.Y, newp.Z);
                            }
                        }
                    }
                    //check(!InFace.Equals(FVector(-1.0f, -1.0f, -1.0f)));
                    //check(!OutFace.Equals(FVector(-1.0f, -1.0f, -1.0f)));
                    if ((FMath::IsNearlyZero(InFace.X, KINDA_SMALL_NUMBER) && 
                        FMath::IsNearlyZero(OutFace.Y, KINDA_SMALL_NUMBER) && 
                        !InFace.Equals(LeftTop, KINDA_SMALL_NUMBER) && 
                        !OutFace.Equals(LeftTop, KINDA_SMALL_NUMBER)) ||
                        (FMath::IsNearlyZero(InFace.Y, KINDA_SMALL_NUMBER) && 
                            FMath::IsNearlyZero(OutFace.X, KINDA_SMALL_NUMBER) && 
                            !InFace.Equals(LeftTop, KINDA_SMALL_NUMBER) && 
                            !OutFace.Equals(LeftTop, KINDA_SMALL_NUMBER)))
                    {
                        AddIfCantFind(Group, LeftTop);
                    }
                    else if ((FMath::IsNearlyEqual(InFace.X, 1.0f * float(Width), KINDA_SMALL_NUMBER) &&
                        FMath::IsNearlyZero(OutFace.Y, KINDA_SMALL_NUMBER) && 
                        !InFace.Equals(RightTop, KINDA_SMALL_NUMBER) && 
                        !OutFace.Equals(RightTop, KINDA_SMALL_NUMBER)) ||
                        (FMath::IsNearlyZero(InFace.Y, KINDA_SMALL_NUMBER) && 
                            FMath::IsNearlyEqual(OutFace.X, 1.0f * float(Width), KINDA_SMALL_NUMBER) &&
                            !InFace.Equals(RightTop, KINDA_SMALL_NUMBER) && 
                            !OutFace.Equals(RightTop, KINDA_SMALL_NUMBER)))
                    {
                        AddIfCantFind(Group, RightTop);
                    }
                    else if ((FMath::IsNearlyZero(InFace.X, KINDA_SMALL_NUMBER) && 
                        FMath::IsNearlyEqual(OutFace.Y, 1.0f * float(Width), KINDA_SMALL_NUMBER) &&
                        !InFace.Equals(LeftBottom, KINDA_SMALL_NUMBER) && 
                        !OutFace.Equals(LeftBottom, KINDA_SMALL_NUMBER)) ||
                        (FMath::IsNearlyEqual(InFace.Y, 1.0f * float(Width), KINDA_SMALL_NUMBER) &&
                            FMath::IsNearlyZero(OutFace.X, KINDA_SMALL_NUMBER) && 
                            !InFace.Equals(LeftBottom, KINDA_SMALL_NUMBER) && 
                            !OutFace.Equals(LeftBottom, KINDA_SMALL_NUMBER)))
                    {
                        AddIfCantFind(Group, LeftBottom);
                    }
                    else if ((FMath::IsNearlyEqual(InFace.Y, 1.0f * float(Width), KINDA_SMALL_NUMBER) &&
                        FMath::IsNearlyEqual(OutFace.X, 1.0f * float(Width), KINDA_SMALL_NUMBER) &&
                        !InFace.Equals(RightBottom, KINDA_SMALL_NUMBER) && 
                        !OutFace.Equals(RightBottom, KINDA_SMALL_NUMBER)) ||
                        (FMath::IsNearlyEqual(InFace.X, 1.0f * float(Width), KINDA_SMALL_NUMBER) &&
                            FMath::IsNearlyEqual(OutFace.Y, 1.0f * float(Width), KINDA_SMALL_NUMBER) &&
                            !InFace.Equals(RightBottom, KINDA_SMALL_NUMBER) && 
                            !OutFace.Equals(RightBottom, KINDA_SMALL_NUMBER)))
                    {
                        AddIfCantFind(Group, RightBottom);
                    }
                    else if ((FMath::IsNearlyZero(InFace.Y, KINDA_SMALL_NUMBER) && 
                        FMath::IsNearlyEqual(OutFace.Y, 1.0f, KINDA_SMALL_NUMBER) && 
                        !InFace.Equals(RightTop, KINDA_SMALL_NUMBER) && 
                        !OutFace.Equals(RightBottom, KINDA_SMALL_NUMBER)) ||
                        (FMath::IsNearlyEqual(InFace.Y, 1.0f, KINDA_SMALL_NUMBER) && 
                            FMath::IsNearlyZero(OutFace.Y, KINDA_SMALL_NUMBER) && 
                            !InFace.Equals(RightBottom, KINDA_SMALL_NUMBER) && 
                            !OutFace.Equals(RightTop, KINDA_SMALL_NUMBER)))
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
            if (s < KINDA_SMALL_NUMBER)
                Group.Empty();
        }
    }

}

// 判断两个线段是否在2D平面内相交
bool UFisheyeCS4CameraRendering::DoSegmentsIntersect(const FVector& p1, const FVector& p2, const FVector& q1, const FVector& q2)
{
    auto Cross = [](const FVector2D& a, const FVector2D& b) {
        return a.X * b.Y - a.Y * b.X;
    };

    auto To2D = [](const FVector& v) {
        return FVector2D(v.X, v.Y); // 投影到XY平面
    };

    FVector2D r = To2D(p2 - p1);
    FVector2D s = To2D(q2 - q1);
    FVector2D pq = To2D(q1 - p1);

    float rxs = Cross(r, s);
    float pqxr = Cross(pq, r);

    if (FMath::IsNearlyZero(rxs)) return false; // 平行或共线

    float t = Cross(pq, s) / rxs;
    float u = pqxr / rxs;

    return (t > 0 && t < 1) && (u > 0 && u < 1);
}

// 判断多边形是否是简单多边形（即不自交）
bool UFisheyeCS4CameraRendering::IsSimplePolygon(const TArray<FVector>& Points)
{
    int32 Num = Points.Num();
    for (int32 i = 0; i < Num; ++i)
    {
        FVector A1 = Points[i];
        FVector A2 = Points[(i + 1) % Num];

        for (int32 j = i + 1; j < Num; ++j)
        {
            // 跳过共顶点或相邻边
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

// 计算2D多边形面积，自动检测是否自交
float UFisheyeCS4CameraRendering::ComputePolygonArea2D(const TArray<FVector>& Points)
{
    int32 NumPoints = Points.Num();
    if (NumPoints < 3) return 0.0f;

    if (!IsSimplePolygon(Points))
    {
        return -1.0f; // 标记非法自交
    }

    float Area = 0.0f;
    for (int32 i = 0; i < NumPoints; ++i)
    {
        const FVector& P1 = Points[i];
        const FVector& P2 = Points[(i + 1) % NumPoints];
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

void UFisheyeCS4CameraRendering::TestSplitPoints()
{
    TArray<FPointInfo> Input;

    //case0
    {
        //// 面 0: x - y = √2
        //Input.Add(FPointInfo(FVector(UE_SQRT_2, 0, 0), { 0 }));
        //Input.Add(FPointInfo(FVector(2, UE_SQRT_2, 0.5), { 0 }));
        //Input.Add(FPointInfo(FVector(1.5, UE_SQRT_2 - 0.5, -0.2), { 0 }));

        //// 面 1: x + y = √2
        //Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, -0.5), { 1 }));
        //Input.Add(FPointInfo(FVector(1, UE_SQRT_2 - 1, 0.2), { 1 }));
        //Input.Add(FPointInfo(FVector(UE_SQRT_2 - 0.5, 0.5, -0.3), { 1 }));

        //// 面 3: z = 1
        //Input.Add(FPointInfo(FVector(0, 0, 1), { 3 }));
        //Input.Add(FPointInfo(FVector(1, 0, 1), { 3 }));
        //Input.Add(FPointInfo(FVector(0, 1, 1), { 3 }));
    }

    //case1
    //{
    //    // 面 0: x - y = √2
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, -UE_SQRT_2 / 2, 0.5), { 0 }));

    //    // 面 1: x + y = √2
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.5), { 1 }));

    //    // 面 3: z = 1
    //    Input.Add(FPointInfo(FVector(0.5, -0.5, -1), { 3 }));
    //    
    //}

    //case2
    //{
    //    // 面 0: x - y = √2
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, -UE_SQRT_2 / 2, 0.5), { 0 }));

    //    //面 0 1 交线
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2, 0, 0.5), { 0,1 }));

    //    // 面 1: x + y = √2
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.5), { 1 }));

    //    // 面 3: z = 1
    //    Input.Add(FPointInfo(FVector(0.5, -0.5, -1), { 3 }));

    //}

    //case3 : 最后两个点情况特殊 , 都在3面上 , 但是一个是13共线一个是03共线 , 这种情况下应该取中间的三面点作为分割点
    //bug, tbd
    //{
    //    Input.Add(FPointInfo(FVector(0.7071, -0.7071, 0.5000), {}));
    //    Input.Add(FPointInfo(FVector(0.7071, 0.7071, 0.5000), {}));
    //    Input.Add(FPointInfo(FVector(0.7071, 0.7071, -1.0000), {}));
    //    Input.Add(FPointInfo(FVector(0.7071, -0.7071, -1.0000), {}));
    //}

    //case4 : 最后两个点情况特殊 , 都在13共线面上 
    //{
    //    Input.Add(FPointInfo(FVector(0.7071, -0.7071, 0.5000), {}));
    //    Input.Add(FPointInfo(FVector(0.7071, 0.7071, 0.5000), {}));
    //    Input.Add(FPointInfo(FVector(0, 1.4142, -1.0000), {}));
    //    Input.Add(FPointInfo(FVector(0.7071, 0.7071, -1.0000), {}));
    //}

    //case5 : 三个在交线上的点构成的三角形. 这里的问题和case3一样 , 问题在于这个时候已经不是生成新交点了 .
    //生成新交点代码能做的是不共面的点根据投影生成穿越多个面的连接线, 找到连接线和面之间的交线的交点 .
    //而这种情况只会发生在下是多个点位于不同的三个面 , 正好把顶点围起来了 . 这时候新增点的工作就结束了 ,
    //需要一个新的函数 , 也就是分割 . 简单的双面情况就是遍历点 , 找到正好处于边界的两个点 , 然后分割.
    //三面就是这种情况 , 需要先确定是哪个顶点 , 然后在遍历点. 找到入面点和出面点后 , 再加上顶点 , 就能分割成三个面
    //{
    //    Input.Add(FPointInfo(FVector(1.4142, 0.0000, 1.0000), {}));
    //    Input.Add(FPointInfo(FVector(0, 1.4142, -1.0000), {}));
    //    Input.Add(FPointInfo(FVector(0, -1.4142, -1.0000), {}));
    //}

    //case6: case5的极端情况
    //{
    //    Input.Add(FPointInfo(FVector(0.707, 0.0000, 1.0000), {}));
    //    Input.Add(FPointInfo(FVector(0, 1.4142, 0), {}));
    //    Input.Add(FPointInfo(FVector(0.707, 0.0000, -1.0000), {}));
    //    Input.Add(FPointInfo(FVector(0, -1.4142, 0), {}));
    //}

    //case7: 
    //{
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, -UE_SQRT_2 / 2, 0.50000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.50000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, -0.50000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, -UE_SQRT_2 / 2, -0.50000), {}));
    //}


    //case8: 
    //{
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, -UE_SQRT_2 / 2, 0.50000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.50000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, 0, -1), {}));
    //}

    //case9: 
    //{
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 * 0.5, -UE_SQRT_2 * 0.5, 0.50000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 * 0.75, -UE_SQRT_2 * 0.25, 0.75000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 * 0.75, UE_SQRT_2 * 0.25, 0.750000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 * 0.5, UE_SQRT_2 * 0.5, 0.50000), {}));
    //    Input.Add(FPointInfo(FVector(0.5, 0.5, -1), {}));
    //    Input.Add(FPointInfo(FVector(0.1, 0, -1), {}));
    //    Input.Add(FPointInfo(FVector(0.5, -0.5, -1), {}));
    //}

    //case10: 
    {
        Input.Add(FPointInfo(FVector(0, -UE_SQRT_2, 1.0000), {}));
        Input.Add(FPointInfo(FVector(UE_SQRT_2, 0, 1.000), {}));
        Input.Add(FPointInfo(FVector(UE_SQRT_2, 0, -1.000), {}));
        Input.Add(FPointInfo(FVector(0, -UE_SQRT_2, -1.0000), {}));

    }

    CheckPointsFaces(Input);
    UE_LOG(LogTemp, Warning, TEXT("=== SplitPoints Before ==="));
    UE_LOG(LogTemp, Warning, TEXT("=== Input ==="));
    for (int i = 0; i < Input.Num(); i++)
    {
        const FVector& P = Input[i].WorldPos;
        UE_LOG(LogTemp, Warning, TEXT("[%d] (%.4f, %.4f, %.4f)"), i, P.X, P.Y, P.Z);
        for (int j = 0; j < Input[i].FaceIndex.Num(); j++)
        {
            FVector local = LocalSpace2Panel(Input[i].FaceIndex[j], P);
            UE_LOG(LogTemp, Warning, TEXT("Local Planel[%d] coord (%.4f, %.4f, %.4f)"), Input[i].FaceIndex[j], local.X, local.Y, local.Z);
        }
    }
    TArray<TArray<FVector>> OutGroups;
    OutGroups.SetNum(4);
    SplitPoints(Input, OutGroups);

    // 打印输出
    if (OutGroups.Num() > 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("=== SplitPoints Result ==="));
        UE_LOG(LogTemp, Warning, TEXT("=== Input ==="));
        for (int i = 0; i < Input.Num(); i++)
        {
            const FVector& P = Input[i].WorldPos;
            UE_LOG(LogTemp, Warning, TEXT("[%d] (%.4f, %.4f, %.4f)"), i, P.X, P.Y, P.Z);
            for (int j = 0; j < Input[i].FaceIndex.Num(); j++)
            {
                FVector local = LocalSpace2Panel(Input[i].FaceIndex[j], P);
                UE_LOG(LogTemp, Warning, TEXT("Local Planel[%d] coord (%.4f, %.4f, %.4f)"), Input[i].FaceIndex[j], local.X, local.Y, local.Z);
            }
        }
        for(int i = 0; i < OutGroups.Num(); i++)
        {
            UE_LOG(LogTemp, Warning, TEXT("=== Plane[%d] ==="), i);
            for (int j = 0; j < OutGroups[i].Num(); j++)
            {
                const FVector& P = OutGroups[i][j];
                UE_LOG(LogTemp, Warning, TEXT("[%d] (%.4f, %.4f, %.4f)"), j, P.X, P.Y, P.Z);
            }
            UE_LOG(LogTemp, Warning, TEXT("Area is %.4f"), ComputePolygonArea2D(OutGroups[i]));
            
        }
        UE_LOG(LogTemp, Warning, TEXT("=========================="));
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("No output groups generated."));
    }
}

void UFisheyeCS4CameraRendering::TestAroundPoints(FVector2D Start, float Size, int n)
{
    TArray<FVector2D> Points;
    if (n <= 0 || Size <= 0.0f) return;

    float Step = 1.0f / float(n);

    for (int i = 0; i < 4 * n; i++)
    {
        int edge = i / n;
        int offset = i % n;

        FVector2D P;

        switch (edge)
        {
        case 0: // 上边 (left → right)
            P = FVector2D(Start.X + float(offset) * Step, Start.Y);
            break;
        case 1: // 右边 (top → bottom)
            P = FVector2D(Start.X + Size, Start.Y + float(offset) * Step);
            break;
        case 2: // 下边 (right → left)
            P = FVector2D(Start.X + Size - float(offset) * Step, Start.Y + Size);
            break;
        case 3: // 左边 (bottom → top)
            P = FVector2D(Start.X, Start.Y + Size - float(offset) * Step);
            break;
        }

        Points.Add(P);
    }

    for(int i = 0; i < Points.Num(); i++)
    {
        UE_LOG(LogTemp, Warning, TEXT("NO.%d , Edge %d , (%.4f, %.4f)"), i, i / n, Points[i].X, Points[i].Y);
    }

    return;
}

void UFisheyeCS4CameraRendering::CalPixelsRelationship(
    FIntPoint Resolution,
    int TextureNum,
    int ProjectionModel)
{
    SnitchNum = TextureNum;
    ID = FString::FromInt(Resolution.X) + TEXT("x") + FString::FromInt(Resolution.Y) + 
        TEXT("_") + TEXT("ProjectType") + FString::FromInt(ProjectionModel) + 
        TEXT("_") + TEXT("SnitchNum") + FString::FromInt(TextureNum);
    Width = Resolution.X;
    Radius = float(Width) / 2;
    UE_LOG(LogTemp, Warning, TEXT("ID is %s"), *ID);
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
        SamplePanelIDptr->Init(-1, Resolution.X * Resolution.Y * TopNPixel);
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
            if (SnitchNum == 4)
            {
                PlaneArray.Add(FPlane(1.0f, -1.0f, 0.0f, UE_SQRT_2 * Radius));
                PlaneArray.Add(FPlane(1.0f, 1.0f, 0.0f, UE_SQRT_2 * Radius));
                PlaneArray.Add(FPlane(0.0f, 0.0f, 1.0f, 1.0f * Radius));
                PlaneArray.Add(FPlane(0.0f, 0.0f, 1.0f, -1.0f * Radius));
            }
            else if (SnitchNum == 5)
            {
                PlaneArray.Add(FPlane(1.0f, 0.0f, 0.0f, 1.0f * Radius));
                PlaneArray.Add(FPlane(0.0f, 1.0f, 0.0f, -1.0f * Radius));
                PlaneArray.Add(FPlane(0.0f, 1.0f, 0.0f, 1.0f * Radius));
                PlaneArray.Add(FPlane(0.0f, 0.0f, 1.0f, 1.0f * Radius));
                PlaneArray.Add(FPlane(0.0f, 0.0f, 1.0f, -1.0f * Radius));
            }

            UE_LOG(LogTemp, Warning, TEXT("LUT can't found in RAM and Disk! Cal"));
            TMap<int, int> AreaCount;
            //O为球心也是3D局部坐标系的原点 , O在原成像面的投影是o , 相距的距离为单位距离1
            //先假设是stereographic投影 , r = 2ftan(θ/2), 暂时f=1
            //这里要注意 , 坐标系是ue4的左手系 , 红色轴x绿色轴y蓝色轴z . 
            //垂直于成像面朝前是x轴 , 成像面水平方向从左到右为y轴, 从下到上为z轴 , 成像面为yz平面
            //设入射光线从点P经过O , 最后落在成像面上的点p(注意大P小p)
            FVector o(-Radius, 0, 0);
            FVector O(0, 0, 0);
            FVector oO = O - o;
            FVector YNormal(0, Radius, 0);
            float Size = 1.0f;
            float Step = Size / float(n);

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
                    FVector Start(float(i), float(j), 0);
                    TArray<FPointInfo> Input;
                    TArray<TArray<FVector>> OutGroups;
                    for (int pidx = 0; pidx < 4 * n; pidx++)
                    {
                        int edge = pidx / n;
                        int offset = pidx % n;
                        
                        FVector2D P;

                        switch (edge)
                        {
                        case 0: // 上边 (left → right)
                            P = FVector2D(Start.X + float(offset) * Step, Start.Y);
                            break;
                        case 1: // 右边 (top → bottom)
                            P = FVector2D(Start.X + Size, Start.Y + float(offset) * Step);
                            break;
                        case 2: // 下边 (right → left)
                            P = FVector2D(Start.X + Size - float(offset) * Step, Start.Y + Size);
                            break;
                        case 3: // 左边 (bottom → top)
                            P = FVector2D(Start.X, Start.Y + Size - float(offset) * Step);
                            break;
                        }
                        TArray<int> SampleCountPanel;
                        SampleCountPanel.Init(0, 5);
                        Samplei = P.X;
                        Samplej = P.Y;

                        int SampleID = pidx;
                        FVector p(-Radius, (Samplej - Radius), (-Samplei + Radius));
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
                            //归一化的空间坐标下的交点 , 注意 , 这时候plane是2x2的平面
                            FVector IntersectPointNormal;
                            if (RayPlaneIntersection(FVector::ZeroVector, 0.5 * OPNormal, PlaneArray[m], IntersectPointNormal) && IsInRange(IntersectPointNormal))
                            {
                                if(Input.Num() == 0)
                                {
                                    Input.Add(FPointInfo(IntersectPointNormal, {m}));
                                }else
                                {
                                    if(Input.Last().WorldPos.Equals(IntersectPointNormal))
                                    {
                                        Input.Last().FaceIndex.Add(m);
                                    }else
                                    {
                                        Input.Add(FPointInfo(IntersectPointNormal, {m}));
                                    }
                                }
                                PixelCountPanel[m]++;
                                SampleCountPanel[m]++;
                                HitPanelCount++;
                                (*FisheyeMaskptr)[i * Resolution.X + j] = 1;
                            }

                        }

                        int SampleSampleCountPanelTotal = 0;
                        for (int a = 0; a < SampleCountPanel.Num(); a++)
                        {
                            SampleSampleCountPanelTotal += SampleCountPanel[a];
                        }
                    
                    }

                    if (Input.Num()>0)
                    {
                        OutGroups.SetNum(SnitchNum);
                        SplitPoints(Input, OutGroups);

                        // 打印输出
                        check(OutGroups.Num());
                        if (OutGroups.Num() > 0)
                        {
                            TArray<TMap<FIntVector, float>> AllPixelMap;
                            float AreaSum = 0.0f;
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
                                AreaSum += Area;

                                int32 Key = FMath::FloorToInt(Area);
                                if (AreaCount.Contains(Key))
                                {
                                    AreaCount[Key]++;
                                }
                                else
                                {
                                    AreaCount.Add(Key, 1);
                                }
                                if (Area < 0.5f)
                                {
                                    continue;
                                }
                                for (int inputidex = 0; inputidex < Input.Num(); inputidex++)
                                {
                                    const FVector& P = Input[inputidex].WorldPos;
                                    for (int faceidx = 0; faceidx < Input[inputidex].FaceIndex.Num(); faceidx++)
                                    {
                                        FVector local = LocalSpace2Panel(Input[inputidex].FaceIndex[faceidx], P);
                                    }
                                }
                                TArray<FIntVector> PixelArray;
                                int PixelCount = 0;
                                int SamplesPerPixel = 10;
                                float MipLV = FMath::Max(0.0f, FMath::Log2(Area) / 2);
                                int MaxMipValue = (SnitchNum == 4) ? 3 : 1;
                                int FloorMipLV = FMath::Clamp(FMath::FloorToInt(MipLV), 0, MaxMipValue);
                                int CeilMipLV = FMath::Clamp(FMath::CeilToInt(MipLV), 0, MaxMipValue);
                                for (int x = AABBXMin; x < AABBXMax; x++)
                                {
                                    for (int y = AABBYMin; y < AABBYMax; y++)
                                    {
                                        float Percentage = 0.0f;
                                        for (int samplei = 0; samplei < SamplesPerPixel; ++samplei)
                                        {
                                            for (int samplej = 0; samplej < SamplesPerPixel; ++samplej)
                                            {
                                                FVector2D samplep = FVector2D(float(x) + 1.0f / float(SamplesPerPixel) * float(samplei), float(y) + 1.0f / float(SamplesPerPixel) * float(samplej));
                                                if (IsPointInPolygon(samplep, OutGroups[groupidx]))
                                                {
                                                    Percentage += 1.0 / (float(SamplesPerPixel)*float(SamplesPerPixel));
                                                }
                                            }
                                        }
                                        if (Percentage > 0.0f)
                                        {
                                            PixelArray.Add(FIntVector(FMath::FloorToInt(x), FMath::FloorToInt(y), 0));
                                            PixelMap.Add(FIntVector(FMath::FloorToInt(x), FMath::FloorToInt(y), 0), Percentage);
                                            PixelCount++;
                                        }
                                    }
                                }

                                //合并像素 , 压缩为高lv的mipmap
                                TArray<FIntVector> PixelArrayToAdd;
                                TSet<FIntVector> PixelArrayToRemove;
                                TMap<FIntVector, float> PixelMapToAdd;

                                // 第一遍：收集要添加/删除的元素
                                for (int l = 0; l < PixelArray.Num(); ++l)
                                {
                                    const FIntVector& CurrentPix = PixelArray[l];

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
                                        }
                                    }
                                }

                                // 第二步：Apply 所有变更
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
                            // 遍历所有面
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

                            // 排序：按权重从大到小排列
                            AllPixels.Sort([](const FPixelInfo& A, const FPixelInfo& B)
                            {
                                return A.Weight > B.Weight; // 大到小
                            });

                            for (int k = 0; k < TopNPixel; k++)
                            {
                                int res;
                                int weight4;
                                int texid;
                                int miplv;
                                int x12;
                                int y12;
                                int w4;
                                float w;
                                if(k >= AllPixels.Num())
                                {
                                    if(SnitchNum == 4)
                                    {
                                        PackToInt32(res, 0, 0, 0, 0, 0);
                                        UnpackFromInt32(res, texid, miplv, x12, y12, w4);
                                    }else if(SnitchNum == 5)
                                    {
                                        PackToInt32_Tex3Bit(res, 0, 0, 0, 0, 0);
                                        UnpackFromInt32_Tex3Bit(res, texid, miplv, x12, y12, w4);
                                    }
                                }else
                                {
                                    weight4 = FMath::Clamp(FMath::RoundToInt(AllPixels[k].Weight * 15.0f), 0, 15);
                                    if (SnitchNum == 4)
                                    {
                                        PackToInt32(res, AllPixels[k].TextureIndex, AllPixels[k].MipLevel, AllPixels[k].X, AllPixels[k].Y, weight4);
                                        UnpackFromInt32(res, texid, miplv, x12, y12, w4);
                                    }
                                    else if (SnitchNum == 5)
                                    {
                                        PackToInt32_Tex3Bit(res, AllPixels[k].TextureIndex, AllPixels[k].MipLevel, AllPixels[k].X, AllPixels[k].Y, weight4);
                                        UnpackFromInt32_Tex3Bit(res, texid, miplv, x12, y12, w4);
                                    }
                                    w = float(w4) / 15.0f;
                                }
                                (*SamplePanelIDptr)[(i * Resolution.X + j) * TopNPixel + k] = res;
                            }
                        }
                        else
                        {
                            UE_LOG(LogTemp, Warning, TEXT("No output groups generated."));
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

        // 判断是否跨越 y=y0 的水平线
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

    if (FMath::IsNearlyZero(Denominator))
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


//注意，由于为了面在坐标轴上下限为[-1,1]方便计算，Plane的设定为2x2大小。所以这里计算到的图像坐标范围为[0,0]到[2,2]，转换为uv坐标系需要除以2
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

bool UFisheyeCS4CameraRendering::SmallerAndEqual(float A, float B, float eps = KINDA_SMALL_NUMBER)
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
        if (SmallerAndEqual(-1.0f * Radius, z) && SmallerAndEqual(z, 1.0f * Radius) && 
            SmallerAndEqual(0.0f, x) && SmallerAndEqual(x, 1.0f * Radius) &&
            SmallerAndEqual(-1.0f * Radius, y) && SmallerAndEqual(y, 1.0f * Radius))
        {
            if( FMath::IsNearlyEqual(y, -1.0f * Radius) || FMath::IsNearlyEqual(y, 1.0f * Radius) ||
                FMath::IsNearlyEqual(z, -1.0f * Radius) || FMath::IsNearlyEqual(z, 1.0f * Radius) || 
                FMath::IsNearlyEqual(x, 1.0f * Radius))
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