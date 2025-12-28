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
     // Corresponding group index.
     int32 TextureIndex;
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
         TArray<FBloomStage>& BloomStages);

     void UseComputeShaderArray_RenderThread(
         FRHICommandListImmediate& RHICmdList,
         TArray<FTextureRenderTargetResource*> InTextureRenderTargetResource,
         FTextureRenderTargetResource* OutTextureRenderTargetResource,
         FTextureRenderTargetResource* MipBloomTextureRenderTargetResource0,
         FIntPoint Resolution,
         int SampleNum);

     void GenFisheyePass(
         FRDGBuilder& GraphBuilder,
         TArray<FRDGTextureRef>& InputTextures,
         FRDGTextureRef OutputHDR);

     void GenMipmapPass(
         FRDGBuilder& GraphBuilder,
         FRDGTextureRef InputTexture,
         FRDGTextureRef OutTexture);

     void GaussBlur1d(
         FRDGBuilder& GraphBuilder,
         FRDGTextureRef InputTexture,
         const FBloomStage& BloomStage,
         int direction);

     void GaussBlur1dAdd(
         FRDGBuilder& GraphBuilder,
         FRDGTextureRef InputTexture,
         FRDGTextureRef InputTextureAdd,
         const FBloomStage& BloomStage,
         int direction);

     void CombineBloom(
         FRDGBuilder& GraphBuilder,
         FRDGTextureRef InputTexture,
         FRDGTextureRef InputBlurTexture,
         FRDGTextureRef InputLUTTexture,
         FRDGTextureRef OutputTexture);

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
         FTextureRenderTargetResource* OutputTextureLDRRenderTargetResource,
         float cx,
         float cy);

     void GeneraLUT_RenderThread(
         FRHICommandListImmediate& RHICmdList);
    
     void CalPixelsRelationship(
         FIntPoint Resolution,
         int TextureNum,
         float MaxAngle,
         float d1,
         float d2,
         float d3,
         float d4,
         float fx,
         float fy,
         float cx,
         float cy);

     void CalGaussian1dKernel(TResourceArray<float>& Gaussian1dKernel, int n, float sd);

     bool IsSampleInCircle(float i, float j, FIntPoint Resolution);

     bool RayPlaneIntersection(FVector RayOrigin, FVector RayDirection, FPlane Plane, FVector& OutHitPoint);

     FVector LocalSpace2Panel(int PanelID, FVector IntersectPoint);

     bool SmallerAndEqual(float A, float B, float eps);

     FVector GetRandomPointOnPlane(const FPlane& Plane);

     bool IsInRange(FVector Point);

     static FTexture2DRHIRef GetSharedLUT(FRHICommandListImmediate& RHICmdList);

     float GetBlurRadius(uint32 ViewSize, float KernelSizePercent);

     void Compute1DGaussianFilterKernel(TResourceArray<float>& Gaussian1dKernel, uint32 SampleCountMax, float KernelRadius);

     float NormalDistributionUnscaled(float X, float Sigma);

     int GetIntegerKernelRadius(uint32 SampleCountMax, float KernelRadius);

     float GetClampedKernelRadius(uint32 SampleCountMax, float KernelRadius);

     virtual void BeginDestroy() override;

     bool SaveResourceArrayToFile(const FString& FilePath, const TResourceArray<int32>& Data);

     bool SaveResourceArrayToFile(const FString& FilePath, const TResourceArray<float>& Data);

     bool LoadResourceArrayFromFile(const FString& FilePath, TResourceArray<int32>& OutData);

     bool LoadResourceArrayFromFile(const FString& FilePath, TResourceArray<float>& OutData);

     int32 FindBinFilesInSavedDir(const FString& MatchString, TArray<FString>& OutFoundFiles);

     FPlane NormalizePlane(const FPlane& P);

     FVector IntersectThreePlanes(const FPlane& P1, const FPlane& P2, const FPlane& P3);

     float GetSegmentTProjection(const FVector& A, const FVector& B, const FVector& P);

     int HasCommonFace(FPointInfo& P1, FPointInfo& P2);

     float ComputePolygonArea2D(const TArray<FVector>& Points);

     bool IsPointOnEdge(FVector Point, float Tolerance);

     bool IsOnSameEdge(FVector P1, FVector P2, float Tolerance);

     bool AddIfCantFind(TArray<FVector>& Group, FVector Point);

     void CheckPointsFaces(TArray<FPointInfo>& InputPoints);

     bool IsPointOnPlane(const FVector& Point, const FPlane& Plane, float Tolerance);

     bool DoSegmentsIntersect(const FVector& p1, const FVector& p2, const FVector& q1, const FVector& q2);

     bool IsSimplePolygon(const TArray<FVector>& Points);

     void SplitPoints(int i, int j, TArray<FPointInfo>& InputPoints, TArray<TArray<FVector>>& OutGroups);

     bool IsPointInPolygon(FVector2D& Point, TArray<FVector>& Polygon);

     FIntPoint GetMipmapCoord(FIntVector Coord);

     static double Halton(int32 Index, int32 Base);

     static TArray<FVector2D> GenerateHalton2DPoints(int32 NumPoints = 15);

     float GetMipArea(int miplv);

 private:
     UPROPERTY(EditAnywhere)
     FString ID;

     static TMap<FString, TSharedPtr<TResourceArray<int>>> MapSamplePanelID;
     static TMap<FString, FStructuredBufferRHIRef> MapSamplePanelIDBuffer;
     static TMap<FString, FShaderResourceViewRHIRef> MapSamplePanelIDSRV;
     static TMap<FString, FRHIResourceCreateInfo*> MapCreateInfoSamplePanelID;

     static TMap<FString, TSharedPtr<TResourceArray<float>>> MapFisheyeMask;
     static TMap<FString, FStructuredBufferRHIRef> MapFisheyeMaskBuffer;
     static TMap<FString, FShaderResourceViewRHIRef> MapFisheyeMaskSRV;
     static TMap<FString, FRHIResourceCreateInfo*> MapFisheyeMaskCreateInfo;

     static FTexture2DRHIRef CreateLUT(FRHICommandListImmediate& RHICmdList);

     static FTexture3DRHIRef CreateLUT3D(FRHICommandListImmediate& RHICmdList);

     int32 Width;
     float Radius;

     int n = 4;
     int TopNPixel = 4;
     int SnitchNum = 5;

     int testi = 674;
     int testj = 671;

     TArray<FPlane> PlaneArray;
 };
