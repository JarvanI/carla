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

UCLASS(BlueprintType, Blueprintable)
class SHADERTESTPLUGIN_API UShadertestRendering : public UObject
{
    GENERATED_BODY()
public:
    UShadertestRendering(const FObjectInitializer& ObjectInitializer);

    void UseComputeShaderArray(
        TArray<UTextureRenderTarget2D*> InputRenderTarget,
        class UTextureRenderTarget2D* OutputRenderTarget,
        int SampleNum,
        int ProjectionModel);

    void UseComputeShaderArray_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        TArray<FTextureRenderTargetResource*> InTextureRenderTargetResource,
        FTextureRenderTargetResource* OutTextureRenderTargetResource,
        FIntPoint Resolution,
        int SampleNum,
        int ProjectionModel);

    void CalPixelsRelationship(
        TResourceArray<int>& PixelInCircle,
        TResourceArray<int>& SamplePanelID,
        TResourceArray<int>& SamplePanelCoord,
        TResourceArray<float>& SamplePanelBrightness,
        FIntPoint Resolution,
        int SampleNum,
        int ProjectionModel);

    //void SetProjectionModel(int ProjectionModel);

    bool IsSampleInCircle(float i, float j, FIntPoint Resolution);

    FVector RayPlaneIntersection(const FVector& RayOrigin, const FVector& RayDirection, const FPlane& Plane, bool& WillIntersection);

    bool IsPointInCube(FVector Point);

    FVector2D LoclSpace2Panel(int PanelID, FVector IntersectPoint, float Radius);

    //int ProjectionModel = 1;
};
