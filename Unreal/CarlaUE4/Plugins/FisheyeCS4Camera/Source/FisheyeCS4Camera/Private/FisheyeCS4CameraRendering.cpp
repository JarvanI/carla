#include "FisheyeCS4CameraRendering.h"

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
#define LOCTEXT_NAMESPACE "FisheyeCS4Camera"

float EPSINON = 0.00001;


// Pack three integer values into a single int32_t
void PackToInt32(int &res, int high4, int mid14, int low14) {
    assert(high4 >= 0 && high4 < (1 << 4));    // Ensure high4 fits in 4 bits
    assert(mid14 >= 0 && mid14 < (1 << 14));   // Ensure mid14 fits in 14 bits
    assert(low14 >= 0 && low14 < (1 << 14));   // Ensure low14 fits in 14 bits

    res = (high4 << 28) | (mid14 << 14) | low14;
}

// Unpack three values from a single int32_t
void UnpackFromInt32(int packed, int &high4, int &mid14, int &low14) {
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

void UFisheyeCS4CameraRendering::UseComputeShaderArray_RenderThread(
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
            static TShaderMapRef<FFisheyeCS4CameraComputeShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            static TResourceArray<int>* SamplePanelID = new TResourceArray<int>();
            static FStructuredBufferRHIRef SamplePanelIDBuffer;
            static FShaderResourceViewRHIRef SamplePanelIDSRV;
            static FRHIResourceCreateInfo CreateInfoSamplePanelID;

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
            }
            Count++;

            UE_LOG(LogTemp, Warning, TEXT("after after SizeX %d ,SizeY %d, SampleNum %d, SamplePanelID %d"),
                SizeX, SizeY, SampleNum, SamplePanelID->Num());
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

void UFisheyeCS4CameraRendering::UseComputeShaderArray(
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
        ENQUEUE_RENDER_COMMAND(FisheyeCS4Camera)
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
        UE_LOG(LogTemp, Log, TEXT("ENQUEUE_RENDER_COMMAND"));
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("no ENQUEUE_RENDER_COMMAND"));
    }
}


void UFisheyeCS4CameraRendering::CalPixelsRelationship(
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
    PlaneArray.Add(FPlane(1, -1, 0, UE_SQRT_2));
    PlaneArray.Add(FPlane(1, 1, 0, UE_SQRT_2));
    PlaneArray.Add(FPlane(0, 0, 1, 1));
    PlaneArray.Add(FPlane(0, 0, 1, -1));

    float SampleDist = 1.0 / (2.0 * float(SampleNum));
    float Radius = FMath::Min(Resolution.X, Resolution.Y) / 2.0;
    SamplePanelID[Resolution.X * Resolution.Y * SampleNum * SampleNum] = layout;
    UE_LOG(LogTemp, Log, TEXT("SamplePanelID[Resolution.X * Resolution.Y * SampleNum * SampleNum] = %d"),
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
                            FVector IntersectPointNormal = RayPlaneIntersection(FVector::ZeroVector, 0.5*OPNormal, PlaneArray[m]);
                            //当找到OP和2D图像的交点
                            if (IsInRange(IntersectPointNormal))
                            {
                                //连续的屏幕坐标 , 坐标原点在左上角 , 竖直朝下是i(x), 水平朝右是j(y)
                                FVector Intersect = LoclSpace2Panel(m, IntersectPointNormal);
                                int X = int(Intersect.X * float(Resolution.X) / 2);
                                int Y = int(Intersect.Y * float(Resolution.Y) / 2);
                                if (X >= Resolution.Y || Y >= Resolution.X)
                                    break;
                                //X = X >= Resolution.Y ? Resolution.Y - 1 : X;
                                //Y = Y >= Resolution.X ? Resolution.X - 1 : Y;
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
                                //SamplePanelID[(i * Resolution.X + j)*SampleNum*SampleNum + SampleID] = m;
                                //SamplePanelCoordX[(i * Resolution.X + j)*SampleNum*SampleNum + SampleID] = X;
                                //SamplePanelCoordY[(i * Resolution.X + j)*SampleNum*SampleNum + SampleID] = Y;
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

            auto GetMaxValue = [](const TArray<int>& Array) -> int {
                check(Array.Num() > 0); // Ensure the array is not empty

                int MaxValue = Array[0];
                for (const int& Value : Array)
                {
                    if (Value > MaxValue)
                    {
                        MaxValue = Value;
                    }
                }
                return MaxValue;
            };
            auto GetTotalValue = [](const TArray<int>& Array) -> int {
                check(Array.Num() > 0); // Ensure the array is not empty
                int Res = 0;
                for (const int& Value : Array)
                {
                    Res += Value;
                }
                return Res;
            };
            int maxpanel = GetMaxValue(PixelCountPanel);
            if (maxpanel > 0)
            {
                int other = GetTotalValue(PixelCountPanel) - maxpanel;
                if (other > 0)
                {
                    //UE_LOG(LogTemp, Error, TEXT("Sample point on other panel num %d , pixel %d,%d"), other, i, j);
                }
                //SamplePanelBrightness[j * Resolution.X + i] = other * 0.0001;
            }
            //if(i == 540 && (j>= 400 && j <= 600))
            //{
            //    UE_LOG(LogTemp, Error, TEXT("pixel %d,%d , hit point on panel %d , pos(%d, %d)"), i, j, 
            //        SamplePanelID[(i*Resolution.X + j)*SampleNum*SampleNum + 8],
            //        SamplePanelCoordX[(i*Resolution.X + j)*SampleNum*SampleNum + 8],
            //        SamplePanelCoordY[(i*Resolution.X + j)*SampleNum*SampleNum + 8]);
            //}
        }
    }
}

bool UFisheyeCS4CameraRendering::IsSampleInCircle(float i, float j, FIntPoint Resolution)
{
    FVector2D SamplePoint(i, j);

    FVector2D ImageCenter(Resolution.X / 2, Resolution.Y / 2);
    float Dist = (ImageCenter - SamplePoint).Size();
    return Dist <= Resolution.X / 2;
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




#undef LOCTEXT_NAMESPACE
#pragma optimize("", on)