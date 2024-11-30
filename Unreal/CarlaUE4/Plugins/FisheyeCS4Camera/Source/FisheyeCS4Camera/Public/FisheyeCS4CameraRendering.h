#pragma once

#include "CoreMinimal.h"
#include "RHI.h"
#include "RHIResources.h"
#include "Containers/DynamicRHIResourceArray.h"
#include "UObject/ObjectMacros.h"
#include "FisheyeCS4CameraRendering.generated.h"

class UTexture2D;
class UTextureRenderTarget2D;
class FTextureRenderTargetResource;

UCLASS(BlueprintType, Blueprintable)
class FISHEYECS4CAMERA_API UFisheyeCS4CameraRendering : public UObject
{
    GENERATED_BODY()
public:
    UFisheyeCS4CameraRendering(const FObjectInitializer& ObjectInitializer);

    void PackToInt32(int &res, int high4, int mid14, int low14);

    // Unpack three values from a single int32_t
    void UnpackFromInt32(int packed, int &high4, int &mid14, int &low14);

    void UseComputeShaderArray(
        TArray<UTextureRenderTarget2D*> InputRenderTarget,
        class UTextureRenderTarget2D* OutputRenderTarget,
        int SampleNum,
        int ProjectionModel,
        int layout);

    void UseComputeShaderArray_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        TArray<FTextureRenderTargetResource*> InTextureRenderTargetResource,
        FTextureRenderTargetResource* OutTextureRenderTargetResource,
        FIntPoint Resolution,
        int SampleNum,
        int ProjectionModel,
        int layout);

    void CalPixelsRelationship(
        TResourceArray<int>& SamplePanelID,
        FIntPoint Resolution,
        int SampleNum,
        int ProjectionModel,
        int layout);

    //void SetProjectionModel(int ProjectionModel);

    bool IsSampleInCircle(float i, float j, FIntPoint Resolution);

    FVector RayPlaneIntersection(FVector RayOrigin, FVector RayDirection, FPlane Plane);

    FVector LoclSpace2Panel(int PanelID, FVector IntersectPoint);

    bool SmallerAndEqual(float A, float B, float eps);

    FVector GetRandomPointOnPlane(const FPlane& Plane);

    bool IsInRange(FVector Point);

    //int ProjectionModel = 1;

    //int Layout = 0;
};
