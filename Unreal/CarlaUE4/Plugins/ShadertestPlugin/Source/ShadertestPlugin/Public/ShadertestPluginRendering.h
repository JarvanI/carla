#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "RHIResources.h"
#include "Containers/DynamicRHIResourceArray.h"
#include "UObject/ObjectMacros.h"
#include "ShadertestPluginRendering.generated.h"

class UTexture2D;
class UTextureRenderTarget2D;
class FTextureRenderTargetResource;

struct FBloomStage
{
    const float Size;
    const FLinearColor& Tint;
};

UCLASS(BlueprintType, Blueprintable)
class SHADERTESTPLUGIN_API UShadertestRendering : public UObject
{
    GENERATED_BODY()
public:
    UShadertestRendering(const FObjectInitializer& ObjectInitializer);

    void PackToInt32(int &res, int high4, int mid14, int low14);

    // Unpack three values from a single int32_t
    void UnpackFromInt32(int packed, int &high4, int &mid14, int &low14);

    void UseComputeShaderArray(
        TArray<UTextureRenderTarget2D*> InputRenderTarget,
        UTextureRenderTarget2D* OutputRenderTarget,
        UTextureRenderTarget2D* OutputRenderTargetLDR,
        TArray<UTextureRenderTarget2D*> MipBloomRenderTarget,
        TArray<FBloomStage>& BloomStages,
        int SampleNum,
        int ProjectionModel,
        int layout);

    void UseComputeShaderArray_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        TArray<FTextureRenderTargetResource*> InTextureRenderTargetResource,
        FTextureRenderTargetResource* OutTextureRenderTargetResource,
        FTextureRenderTargetResource* MipBloomTextureRenderTargetResource0,
        FIntPoint Resolution,
        int SampleNum,
        int ProjectionModel,
        int layout);

    void GenMipmap_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        FTextureRenderTargetResource* InputTextureRenderTargetResource,
        FTextureRenderTargetResource* OutTextureRenderTargetResource);

    void GaussianBlur(
        FRHICommandListImmediate& RHICmdList,
        FTextureRenderTargetResource* InTextureRenderTargetResource,
        FBloomStage& BloomStage,
        int direction);

    void GaussianBlurAdd(
        FRHICommandListImmediate& RHICmdList,
        FTextureRenderTargetResource* InTextureRenderTargetResource,
        FTextureRenderTargetResource* InAddTextureRenderTargetResource,
        FBloomStage& BloomStage,
        int direction);

    void Upscaling_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        FTextureRenderTargetResource* InputHiResOriTextureRenderTargetResource,
        FTextureRenderTargetResource* InputLowBlurTextureRenderTargetResource);

    void CombineBloom_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        FTextureRenderTargetResource* InputOriTextureRenderTargetResource,
        FTextureRenderTargetResource* InputBlurTextureRenderTargetResource,
        FTextureRenderTargetResource* OutputTextureLDRRenderTargetResource);

    void GeneraLUT_RenderThread(
        FRHICommandListImmediate& RHICmdList);

    void CalPixelsRelationship(
        TResourceArray<int>& SamplePanelID,
        FIntPoint Resolution,
        int SampleNum,
        int ProjectionModel,
        int layout);

    void CalGaussian1dKernel(TResourceArray<float>& Gaussian1dKernel, int n, float sd);

    //void SetProjectionModel(int ProjectionModel);

    bool IsSampleInCircle(float i, float j, FIntPoint Resolution);

    FVector RayPlaneIntersection(const FVector& RayOrigin, const FVector& RayDirection, const FPlane& Plane, bool& WillIntersection);

    bool IsPointInCube(FVector Point);

    FVector2D LoclSpace2Panel(int PanelID, FVector IntersectPoint, float Radius);

    //int ProjectionModel = 1;

    static FTexture2DRHIRef GetSharedLUT(FRHICommandListImmediate& RHICmdList);

private:
    static TResourceArray<int> PixelInCircle;

    static FTexture2DRHIRef CreateLUT(FRHICommandListImmediate& RHICmdList);

    static FTexture3DRHIRef CreateLUT3D(FRHICommandListImmediate& RHICmdList);
};
