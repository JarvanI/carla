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

struct FPointInfo
{
    FVector WorldPos;
    TArray<int32> FaceIndex;

    FPointInfo(const FVector& InPos, TArray<int32> InFace)
        : WorldPos(InPos), FaceIndex(InFace) {}
};


struct FSegment
{
    FPointInfo PStart;
    FPointInfo PEnd;

    FSegment(FPointInfo& P1, FPointInfo& P2)
        : PStart(P1), PEnd(P2) {}
};


UCLASS(BlueprintType, Blueprintable)
class FISHEYECS4CAMERA_API UFisheyeCS4CameraRendering : public UObject
{
    GENERATED_BODY()
public:
    struct FBloomStage
    {
        const float Size;
        const FLinearColor& Tint;
    };

    UFisheyeCS4CameraRendering(const FObjectInitializer& ObjectInitializer);

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

    void CombineBloom_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        FTextureRenderTargetResource* InputOriTextureRenderTargetResource,
        FTextureRenderTargetResource* InputBlurTextureRenderTargetResource,
        FTextureRenderTargetResource* OutputTextureLDRRenderTargetResource);

    void GeneraLUT_RenderThread(
        FRHICommandListImmediate& RHICmdList);
    
    void CalPixelsRelationship(
        FIntPoint Resolution,
        int SampleNum,
        int ProjectionModel,
        int layout);

    void CalGaussian1dKernel(TResourceArray<float>& Gaussian1dKernel, int n, float sd);

    bool IsSampleInCircle(float i, float j, FIntPoint Resolution);

    FVector RayPlaneIntersection(FVector RayOrigin, FVector RayDirection, FPlane Plane);

    FVector LocalSpace2Panel(int PanelID, FVector IntersectPoint);

    bool SmallerAndEqual(float A, float B, float eps);

    FVector GetRandomPointOnPlane(const FPlane& Plane);

    bool IsInRange(FVector Point);

    //int ProjectionModel = 1;

    static FTexture2DRHIRef GetSharedLUT(FRHICommandListImmediate& RHICmdList);

    float GetBlurRadius(uint32 ViewSize, float KernelSizePercent);

    void Compute1DGaussianFilterKernel(TResourceArray<float>& Gaussian1dKernel, uint32 SampleCountMax, float KernelRadius);

    float NormalDistributionUnscaled(float X, float Sigma);

    int GetIntegerKernelRadius(uint32 SampleCountMax, float KernelRadius);

    float GetClampedKernelRadius(uint32 SampleCountMax, float KernelRadius);

    virtual void BeginDestroy() override;

    bool SaveResourceArrayToFile(const FString& FilePath, const TResourceArray<int32>& Data);

    bool LoadResourceArrayFromFile(const FString& FilePath, TResourceArray<int32>& OutData);

    int32 FindBinFilesInSavedDir(const FString& MatchString, TArray<FString>& OutFoundFiles);

    void TestResourceArraySerialization();

    FPlane NormalizePlane(const FPlane& P);

    FVector IntersectThreePlanes(const FPlane& P1, const FPlane& P2, const FPlane& P3);

    float GetSegmentTProjection(const FVector& A, const FVector& B, const FVector& P);

    int HasCommonFace(FPointInfo& P1, FPointInfo& P2);

    float ComputePolygonArea2D(const TArray<FVector>& Points);

    bool IsPointOnEdge(FVector Point);

    bool IsOnSameEdge(FVector P1, FVector P2);

    bool AddIfCantFind(TArray<FVector>& Group, FVector Point);

    void CheckPointsFaces(TArray<FPointInfo>& InputPoints);

    bool IsPointOnPlane(const FVector& Point, const FPlane& Plane, float Tolerance);

    bool DoSegmentsIntersect(const FVector& p1, const FVector& p2, const FVector& q1, const FVector& q2);

    bool IsSimplePolygon(const TArray<FVector>& Points);

    void SplitPoints(TArray<FPointInfo>& InputPoints, TArray<TArray<FVector>>& OutGroups);

    void TestSplitPoints();

private:
    UPROPERTY(EditAnywhere)
    FString ID;

    static TMap<FString, TSharedPtr<TResourceArray<int>>> MapSamplePanelID;
    static TMap<FString, FStructuredBufferRHIRef> MapSamplePanelIDBuffer;
    static TMap<FString, FShaderResourceViewRHIRef> MapSamplePanelIDSRV;
    static TMap<FString, FRHIResourceCreateInfo*> MapCreateInfoSamplePanelID;

    UPROPERTY(EditAnywhere)
    int32 Width;
    static TMap<int32, TSharedPtr<TResourceArray<int>>> MapFisheyeMask;
    static TMap<int32, FStructuredBufferRHIRef> MapFisheyeMaskBuffer;
    static TMap<int32, FShaderResourceViewRHIRef> MapFisheyeMaskSRV;
    static TMap<int32, FRHIResourceCreateInfo*> MapFisheyeMaskCreateInfo;


    static FTexture2DRHIRef CreateLUT(FRHICommandListImmediate& RHICmdList);

    static FTexture3DRHIRef CreateLUT3D(FRHICommandListImmediate& RHICmdList);
};
