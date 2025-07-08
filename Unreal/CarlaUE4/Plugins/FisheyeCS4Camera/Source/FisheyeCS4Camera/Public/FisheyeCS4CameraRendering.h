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

    FPointInfo()
        : WorldPos(FVector::ZeroVector), FaceIndex() {}
};


struct FSegment
{
    FPointInfo PStart;
    FPointInfo PEnd;

    FSegment(FPointInfo& P1, FPointInfo& P2)
        : PStart(P1), PEnd(P2) {}
};


struct FPixelInfo
{
    int32 TextureIndex;  // 对应 groupidx
    int32 X;
    int32 Y;
    int32 MipLevel;
    float Weight;

    FPixelInfo(int32 InTexIdx, int32 InX, int32 InY, int32 InMip, float InWeight)
        : TextureIndex(InTexIdx), X(InX), Y(InY), MipLevel(InMip), Weight(InWeight) {}
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

    void PackToInt32(int &res, int texidx2, int miplv2, int x12, int y12, int weight4);

    // Unpack three values from a single int32_t
    void UnpackFromInt32(int res, int &texidx2, int &miplv2, int &x12, int &y12, int &weight4);

    // Pack with texidx using 3 bits, miplv using 1 bit
    void PackToInt32_Tex3Bit(int& res, int texidx3, int miplv1, int x12, int y12, int weight4);

    // Unpack with texidx using 3 bits, miplv using 1 bit
    void UnpackFromInt32_Tex3Bit(int res, int& texidx3, int& miplv1, int& x12, int& y12, int& weight4);

    void UseComputeShaderArray(
        TArray<UTextureRenderTarget2D*> InputRenderTarget,
        UTextureRenderTarget2D* OutputRenderTarget,
        UTextureRenderTarget2D* OutputRenderTargetLDR,
        TArray<UTextureRenderTarget2D*> MipBloomRenderTarget,
        TArray<FBloomStage>& BloomStages,
        int SampleNum,
        int ProjectionModel);

    void UseComputeShaderArray_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        TArray<FTextureRenderTargetResource*> InTextureRenderTargetResource,
        FTextureRenderTargetResource* OutTextureRenderTargetResource,
        FTextureRenderTargetResource* MipBloomTextureRenderTargetResource0,
        FIntPoint Resolution,
        int SampleNum,
        int ProjectionModel);

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
        int TextureNum,
        int ProjectionModel);

    void CalGaussian1dKernel(TResourceArray<float>& Gaussian1dKernel, int n, float sd);

    bool IsSampleInCircle(float i, float j, FIntPoint Resolution);

    bool RayPlaneIntersection(FVector RayOrigin, FVector RayDirection, FPlane Plane, FVector& OutHitPoint);

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

    bool IsPointInPolygon(FVector2D& Point, TArray<FVector>& Polygon);

    void TestSplitPoints();

    void TestAroundPoints(FVector2D Start, float Size, int n);

    FIntPoint GetMipmapCoord(FIntVector Coord);

private:
    UPROPERTY(EditAnywhere)
    FString ID;

    static TMap<FString, TSharedPtr<TResourceArray<int>>> MapSamplePanelID;
    static TMap<FString, FStructuredBufferRHIRef> MapSamplePanelIDBuffer;
    static TMap<FString, FShaderResourceViewRHIRef> MapSamplePanelIDSRV;
    static TMap<FString, FRHIResourceCreateInfo*> MapCreateInfoSamplePanelID;

    UPROPERTY(EditAnywhere)
    int32 Width;
    float Radius;
    static TMap<int32, TSharedPtr<TResourceArray<int>>> MapFisheyeMask;
    static TMap<int32, FStructuredBufferRHIRef> MapFisheyeMaskBuffer;
    static TMap<int32, FShaderResourceViewRHIRef> MapFisheyeMaskSRV;
    static TMap<int32, FRHIResourceCreateInfo*> MapFisheyeMaskCreateInfo;


    static FTexture2DRHIRef CreateLUT(FRHICommandListImmediate& RHICmdList);

    static FTexture3DRHIRef CreateLUT3D(FRHICommandListImmediate& RHICmdList);

    int n = 4;
    int TopNPixel = 4;
    int SnitchNum = 5;

    TArray<FPlane> PlaneArray;
};
