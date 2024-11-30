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

class FNewMyComputeShader : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FNewMyComputeShader, Global)

public:
    FNewMyComputeShader() {}
    FNewMyComputeShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
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
IMPLEMENT_SHADER_TYPE(, FNewMyComputeShader, TEXT("/Plugin/ShadertestPlugin/Private/TexturePacker.usf"), TEXT("MainCS"), SF_Compute)

void UShadertestRendering::UseComputeShaderArray_RenderThread(
    FRHICommandListImmediate& RHICmdList,
    TArray<FTextureRenderTargetResource*> InTextureRenderTargetResource,
    FTextureRenderTargetResource* OutTextureRenderTargetResource,
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
        if (OutRenderTargetTexture.IsValid())
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
                PF_B8G8R8A8, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, CreateInfo);
            //创建贴图资源的UAV视图
            FUnorderedAccessViewRHIRef TextureUAV = RHICreateUnorderedAccessView(CreatedRHITexture);
            //FUnorderedAccessViewRHIRef TextureUAV = RHICreateUnorderedAccessView(OutRenderTargetTexture);
            TRefCountPtr<FRHITexture> NewOutRenderTargetTexture2(CreatedRHITexture);

            TArray<TRefCountPtr<FRHITexture>> NewInRenderTargetTexture;
            for (int i = 0; i < InRenderTargetTexture.Num(); i++)
            {
                NewInRenderTargetTexture.Add(TRefCountPtr<FRHITexture>(InRenderTargetTexture[i]));
            }
            TRefCountPtr<FRHITexture> NewOutRenderTargetTexture(OutRenderTargetTexture);

            static uint32 Count = 0;
            static TShaderMapRef<FNewMyComputeShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            static TResourceArray<int>* SamplePanelID = new TResourceArray<int>();
            static FStructuredBufferRHIRef SamplePanelIDBuffer;
            static FShaderResourceViewRHIRef SamplePanelIDSRV;
            static FRHIResourceCreateInfo CreateInfoSamplePanelID;

            static int OldProjectionModel = -1;
            static int OldLayout = -1;
            if (Count == 0)
            {
                UE_LOG(LogTemp, Warning, TEXT("if(Count == 0)"));
                SamplePanelID->Init(-1, SizeX * SizeY * SampleNum * SampleNum + 1);
                UE_LOG(LogTemp, Warning, TEXT("before SizeX %d ,SizeY %d, SampleNum %d, SamplePanelID %d"),
                    SizeX, SizeY, SampleNum, SamplePanelID->Num());
                CalPixelsRelationship(*SamplePanelID, Resolution, SampleNum, ProjectionModel, layout);
                UE_LOG(LogTemp, Warning, TEXT("after SizeX %d ,SizeY %d, SampleNum %d, SamplePanelID %d"),
                    SizeX, SizeY, SampleNum, SamplePanelID->Num());

                CreateInfoSamplePanelID.ResourceArray = SamplePanelID;
                SamplePanelIDBuffer = RHICreateStructuredBuffer(sizeof(int), sizeof(int) * SamplePanelID->Num(),
                    BUF_Static | BUF_ShaderResource, CreateInfoSamplePanelID);
                SamplePanelIDSRV = RHICreateShaderResourceView(SamplePanelIDBuffer);

                OldProjectionModel = ProjectionModel;
                OldLayout = layout;
            }
            Count++;

            UE_LOG(LogTemp, Warning, TEXT("after after SizeX %d ,SizeY %d, SampleNum %d, SamplePanelID %d"),
                SizeX, SizeY, SampleNum, SamplePanelID->Num());
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
            ComputeShader->SetParameters(RHICmdList, NewInRenderTargetTexture,
                NewOutRenderTargetTexture2, TextureUAV,
                SamplerState, SamplePanelIDSRV);

            RHICmdList.TransitionResource(
                EResourceTransitionAccess::ERWNoBarrier,
                EResourceTransitionPipeline::EGfxToCompute,
                TextureUAV);
            DispatchComputeShader(RHICmdList, *ComputeShader, GroupSizeX, GroupSizeY, 1);

            //把CS输出的UAV贴图拷贝到RenderTargetTexture
            RHICmdList.CopyTexture(CreatedRHITexture, OutRenderTargetTexture, FRHICopyTextureInfo());
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

void UShadertestRendering::UseComputeShaderArray(
    TArray<UTextureRenderTarget2D*> InputRenderTarget,
    class UTextureRenderTarget2D* OutputRenderTarget,
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

    TArray<FTextureRenderTargetResource*> InputTextureRenderTargetResource;
    for (int i = 0; i < InputRenderTarget.Num(); i++)
    {
        InputTextureRenderTargetResource.Add(InputRenderTarget[i]->GameThread_GetRenderTargetResource());
    }
    FTextureRenderTargetResource* OutTextureRenderTargetResource = OutputRenderTarget->GameThread_GetRenderTargetResource();
    Resolution.X = InputTextureRenderTargetResource[0]->GetSizeX();
    Resolution.Y = InputTextureRenderTargetResource[0]->GetSizeY();

    if (OutTextureRenderTargetResource)
    {
        ENQUEUE_RENDER_COMMAND(FisheyeCSCamera)
            (
                [&](FRHICommandListImmediate& RHICmdList)
        {
            UseComputeShaderArray_RenderThread
            (
                RHICmdList,
                InputTextureRenderTargetResource,
                OutTextureRenderTargetResource,
                Resolution,
                SampleNum,
                ProjectionModel,
                layout
            );
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
    UE_LOG(LogTemp, Log, TEXT("UShadertestRendering::SamplePanelID[Resolution.X * Resolution.Y * SampleNum * SampleNum] = %d"),
        SamplePanelID[Resolution.X * Resolution.Y * SampleNum * SampleNum]);
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