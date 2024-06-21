#include "NewShaderPluginRendering.h"
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
#define LOCTEXT_NAMESPACE "NewShaderPlugin"

UNewShaderRendering::UNewShaderRendering(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
}

class FNewShaderComputeShader : public FGlobalShader
{
    DECLARE_SHADER_TYPE(FNewShaderComputeShader, Global)

public:
    FNewShaderComputeShader() {}
    FNewShaderComputeShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
        : FGlobalShader(Initializer)
    {
        InputTexture.Bind(Initializer.ParameterMap, TEXT("InputTexture"));
        RWOutputTexture.Bind(Initializer.ParameterMap, TEXT("OutputTexture"));
        InputTextureSampler.Bind(Initializer.ParameterMap, TEXT("InputTextureSampler"));
        PixelInCircle.Bind(Initializer.ParameterMap, TEXT("PixelInCircle"));
        SamplePanelID.Bind(Initializer.ParameterMap, TEXT("SamplePanelID"));
        SamplePanelCoord.Bind(Initializer.ParameterMap, TEXT("SamplePanelCoord"));
        SamplePanelBrightness.Bind(Initializer.ParameterMap, TEXT("SamplePanelBrightness"));
    }

    void SetParameters(
        FRHICommandListImmediate& RHICmdList,
        TArray<TRefCountPtr<FRHITexture>> InputTextureRef,
        FTextureRHIRef& OutTextureRef,
        FUnorderedAccessViewRHIRef& OutputTextureUAVRef,
        FSamplerStateRHIRef SamplerState,
        FShaderResourceViewRHIRef& PixelInCircleSRV,
        FShaderResourceViewRHIRef& SamplePanelIDSRV,
        FShaderResourceViewRHIRef& SamplePanelCoordSRV,
        FShaderResourceViewRHIRef& SamplePanelBrightnessSRV)
    {
        for(int i = 0; i < InputTextureRef.Num(); i++)
        {
            if(InputTextureRef.IsValidIndex(i))
            {
                RHICmdList.SetShaderTexture(GetComputeShader(),InputTexture.GetBaseIndex() + i + 1,InputTextureRef[i]);
            }else
            {
                UE_LOG(LogTemp, Error, TEXT("InputTextureRef.IsValidIndex(%d)"),i);
            }
        }
        RHICmdList.SetShaderSampler(GetComputeShader(), InputTextureSampler.GetBaseIndex(), SamplerState);
        RWOutputTexture.SetTexture(RHICmdList, GetComputeShader(), OutTextureRef, OutputTextureUAVRef);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), PixelInCircle.GetBaseIndex(), PixelInCircleSRV);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), SamplePanelID.GetBaseIndex(), SamplePanelIDSRV);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), SamplePanelCoord.GetBaseIndex(), SamplePanelCoordSRV);
        RHICmdList.SetShaderResourceViewParameter(GetComputeShader(), SamplePanelBrightness.GetBaseIndex(), SamplePanelBrightnessSRV);
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
        Ar << InputTextureSampler;
        Ar << PixelInCircle;
        Ar << SamplePanelID;
        Ar << SamplePanelCoord;
        Ar << SamplePanelBrightness;
        return bShaderHasOutdatedParameters;
    }

private:
    FShaderResourceParameter InputTexture;
    FRWShaderParameter RWOutputTexture;
    FShaderResourceParameter InputTextureSampler;
    FShaderResourceParameter PixelInCircle;
    FShaderResourceParameter SamplePanelID;
    FShaderResourceParameter SamplePanelCoord;
    FShaderResourceParameter SamplePanelBrightness;
};
IMPLEMENT_SHADER_TYPE(, FNewShaderComputeShader, TEXT("/Plugin/NewShaderPlugin/Private/TexturePacker.usf"), TEXT("MainCS"), SF_Compute)

void UNewShaderRendering::UseComputeShaderArray_RenderThread(
    FRHICommandListImmediate& RHICmdList,
    TArray<FTextureRenderTargetResource*> InTextureRenderTargetResource,
    FTextureRenderTargetResource* OutTextureRenderTargetResource,
    FIntPoint Resolution,
    int SampleNum)
{
    check(IsInRenderingThread());
    if (OutTextureRenderTargetResource)
    {
        TArray<FTexture2DRHIRef> InRenderTargetTexture;
        for(int i = 0; i < InTextureRenderTargetResource.Num(); i++)
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
            uint32 GroupSizeX = FMath::DivideAndRoundUp((uint32)SizeX, GroupSize);
            uint32 GroupSizeY = FMath::DivideAndRoundUp((uint32)SizeY, GroupSize);
            UE_LOG(LogTemp, Warning, TEXT("SizeX %d ,SizeY %d, GroupSizeX %d, GroupSizeY %d"), SizeY, SizeY, GroupSizeX, GroupSizeY);
            //创建一个贴图资源
            FRHIResourceCreateInfo CreateInfo;
            FTexture2DRHIRef CreatedRHITexture = RHICreateTexture2D(SizeY, SizeY,
                PF_B8G8R8A8, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, CreateInfo);
            //创建贴图资源的UAV视图
            FUnorderedAccessViewRHIRef TextureUAV = RHICreateUnorderedAccessView(CreatedRHITexture);
            TRefCountPtr<FRHITexture> NewOutRenderTargetTexture2(CreatedRHITexture);

            TArray<TRefCountPtr<FRHITexture>> NewInRenderTargetTexture;
            for (int i = 0; i < InRenderTargetTexture.Num(); i++)
            {
                NewInRenderTargetTexture.Add(TRefCountPtr<FRHITexture>(InRenderTargetTexture[i]));
            }
            TRefCountPtr<FRHITexture> NewOutRenderTargetTexture(OutRenderTargetTexture);

            static uint32 Count = 0;
            static TShaderMapRef<FNewShaderComputeShader> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

            static TResourceArray<int>* PixelInCircle = new TResourceArray<int>();
            static FStructuredBufferRHIRef PixelInCircleBuffer;
            static FShaderResourceViewRHIRef PixelInCircleSRV;
            static FRHIResourceCreateInfo CreateInfoPixelInCircle;

            static TResourceArray<int>* SamplePanelID = new TResourceArray<int>();
            static FStructuredBufferRHIRef SamplePanelIDBuffer;
            static FShaderResourceViewRHIRef SamplePanelIDSRV;
            static FRHIResourceCreateInfo CreateInfoSamplePanelID;

            static TResourceArray<int>* SamplePanelCoord = new TResourceArray<int>();
            static FStructuredBufferRHIRef SamplePanelCoordBuffer;
            static FShaderResourceViewRHIRef SamplePanelCoordSRV;
            static FRHIResourceCreateInfo CreateInfoSamplePanelCoord;

            static TResourceArray<float>* SamplePanelBrightness = new TResourceArray<float>();
            static FStructuredBufferRHIRef SamplePanelBrightnessBuffer;
            static FShaderResourceViewRHIRef SamplePanelBrightnessSRV;
            static FRHIResourceCreateInfo CreateInfoSamplePanelBrightness;
            if(Count == 0)
            {
                Count++;
                return;
            }
            else if (Count == 1)
            {
                UE_LOG(LogTemp, Error, TEXT("if(Count == 0)"));

                PixelInCircle->Init(0, SizeY * SizeY);
                SamplePanelID->Init(-1, SizeY * SizeY * SampleNum * SampleNum * 2);
                SamplePanelCoord->Init(0, SizeY * SizeY * SampleNum * SampleNum * 2 * 2);
                SamplePanelBrightness->Init(0.0, SizeY * SizeY);
                UE_LOG(LogTemp, Error, TEXT("in if before SizeX %d ,SizeY %d, SampleNum %d, SamplePanelID %d, SamplePanelCoord %d"),
                    SizeY, SizeY, SampleNum, SamplePanelID->Num(), SamplePanelCoord->Num());
                CalPixelsRelationship(*PixelInCircle, *SamplePanelID, *SamplePanelCoord, *SamplePanelBrightness, FIntPoint(SizeY, SizeY), SampleNum);
                UE_LOG(LogTemp, Error, TEXT("in if after SizeX %d ,SizeY %d, SampleNum %d, SamplePanelID %d, SamplePanelCoord %d"),
                    SizeY, SizeY, SampleNum, SamplePanelID->Num(), SamplePanelCoord->Num());

                CreateInfoPixelInCircle.ResourceArray = PixelInCircle;
                PixelInCircleBuffer = RHICreateStructuredBuffer(sizeof(int), sizeof(int) * PixelInCircle->Num(),
                    BUF_Static | BUF_ShaderResource, CreateInfoPixelInCircle);
                PixelInCircleSRV = RHICreateShaderResourceView(PixelInCircleBuffer);

                CreateInfoSamplePanelID.ResourceArray = SamplePanelID;
                SamplePanelIDBuffer = RHICreateStructuredBuffer(sizeof(int), sizeof(int) * SamplePanelID->Num(),
                    BUF_Static | BUF_ShaderResource, CreateInfoSamplePanelID);
                SamplePanelIDSRV = RHICreateShaderResourceView(SamplePanelIDBuffer);

                CreateInfoSamplePanelCoord.ResourceArray = SamplePanelCoord;
                SamplePanelCoordBuffer = RHICreateStructuredBuffer(sizeof(float), sizeof(float) * SamplePanelCoord->Num(),
                    BUF_Static | BUF_ShaderResource, CreateInfoSamplePanelCoord);
                SamplePanelCoordSRV = RHICreateShaderResourceView(SamplePanelCoordBuffer);

                CreateInfoSamplePanelBrightness.ResourceArray = SamplePanelBrightness;
                SamplePanelBrightnessBuffer = RHICreateStructuredBuffer(sizeof(float), sizeof(float) * SamplePanelBrightness->Num(),
                    BUF_Static | BUF_ShaderResource, CreateInfoSamplePanelBrightness);
                SamplePanelBrightnessSRV = RHICreateShaderResourceView(SamplePanelBrightnessBuffer);

                //for(uint32 i = 0; i < SizeY; i++)
                //{
                //    for (uint32 j = 0; j < SizeY; j++)
                //    {
                //        UE_LOG(LogTemp, Error, TEXT("Pixel %d , %d is in Pixel ? %d "), i, j, (*PixelInCircle)[j * SizeY + i]);
                //    }
                //}
            }
            Count++;

            UE_LOG(LogTemp, Warning, TEXT("in if out SizeX %d ,SizeY %d, SampleNum %d, SamplePanelID %d, SamplePanelCoord %d"),
                SizeY, SizeY, SampleNum, SamplePanelID->Num(), SamplePanelCoord->Num());
            RHICmdList.SetComputeShader(ComputeShader->GetComputeShader());

            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Bilinear>::GetRHI();
            // 将参数传递给ComputeShader
            //这里我们实际上能用到的是UAV,追查到SetTexture函数我们可以发现，对于ComputeShader，第二个参数实际上是没有用的
            ComputeShader->SetParameters(RHICmdList, NewInRenderTargetTexture, 
                NewOutRenderTargetTexture2, TextureUAV, SamplerState, PixelInCircleSRV,
                SamplePanelIDSRV, SamplePanelCoordSRV, SamplePanelBrightnessSRV);

            RHICmdList.TransitionResource(
                EResourceTransitionAccess::ERWNoBarrier,
                EResourceTransitionPipeline::EGfxToCompute,
                TextureUAV);
            DispatchComputeShader(RHICmdList, *ComputeShader, GroupSizeY, GroupSizeY, 1);

            //把CS输出的UAV贴图拷贝到RenderTargetTexture
            RHICmdList.CopyTexture(CreatedRHITexture, OutRenderTargetTexture, FRHICopyTextureInfo());
            UE_LOG(LogTemp, Log, TEXT("UseComputeShader_RenderThread : Texture Size: %d x %d"), SizeY, SizeY);
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

void UNewShaderRendering::UseComputeShaderArray(
    TArray<UTextureRenderTarget2D*> InputRenderTarget,
    UTextureRenderTarget2D* OutputRenderTarget,
    int SampleNum)
{
    check(IsInGameThread());
    

    if (!OutputRenderTarget)
    {
        UE_LOG(LogTemp, Error, TEXT("no OutputRenderTarget"));
        return;
    }

    FIntPoint Resolution;
    TArray<FTextureRenderTargetResource*> InputTextureRenderTargetResource;
    for (int i = 0; i < InputRenderTarget.Num(); i++)
    {
        InputTextureRenderTargetResource.Add(InputRenderTarget[i]->GameThread_GetRenderTargetResource());
    }
    FTextureRenderTargetResource* OutTextureRenderTargetResource = OutputRenderTarget->GameThread_GetRenderTargetResource();
    Resolution.X = InputTextureRenderTargetResource[0]->GetSizeX();
    Resolution.Y = InputTextureRenderTargetResource[0]->GetSizeX();

    if (OutTextureRenderTargetResource)
    {
        ENQUEUE_RENDER_COMMAND(CaptureCommand)
            (
                [&](FRHICommandListImmediate& RHICmdList)
        {
            UseComputeShaderArray_RenderThread
            (
                RHICmdList,
                InputTextureRenderTargetResource,
                OutTextureRenderTargetResource,
                Resolution,
                SampleNum
            );
        }
        );
        FlushRenderingCommands();
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("no ENQUEUE_RENDER_COMMAND"));
    }
}


void UNewShaderRendering::CalPixelsRelationship(
    TResourceArray<int>& PixelInCircle,
    TResourceArray<int>& SamplePanelID, 
    TResourceArray<int>& SamplePanelCoord, 
    TResourceArray<float>& SamplePanelBrightness,
    FIntPoint Resolution, 
    int SampleNum)
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

    for (int i = 0; i < Resolution.X; i++)
    {
        for (int j = 0; j < Resolution.Y; j++)
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
                        PixelInCircle[j * Resolution.X + i] = 1;
                        FVector p(-1, (Samplej - Radius) / Radius, (-Samplei + Radius) / Radius);
                        FVector po = o - p;
                        FVector pO = O - p;
                        //thetad是pO和oO的夹角 , 也就是逆向的出射光线和x轴正向的夹角
                        float thetad = FMath::Acos(FVector::DotProduct(oO, pO) / (oO.Size() * pO.Size()));
                        //theta是OP和oO轴的夹角 , 也就是逆向的入射光线和x轴正向的夹角
                        float theta;
                        switch (4)
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
                            //FVector IntersectPointNormal = FMath::RayPlaneIntersection(FVector::ZeroVector, OPNormal, PlaneArray[m]);

                            //当找到OP和2D图像的交点
                            if (WillIntersect && IsPointInCube(IntersectPointNormal))
                            {
                                //局部空间坐标
                                FVector IntersectPoint = IntersectPointNormal * Radius;
                                //连续的屏幕坐标 , 坐标原点在左上角 , 竖直朝下是i(x), 水平朝右是j(y)
                                FVector2D IncidentRayOrigin = LoclSpace2Panel(m, IntersectPoint, Radius);
                                IncidentRayOrigin.X = IncidentRayOrigin.X >= 1080.0f ? IncidentRayOrigin.X - 1 : IncidentRayOrigin.X;
                                IncidentRayOrigin.Y = IncidentRayOrigin.Y >= 1080.0f ? IncidentRayOrigin.Y - 1 : IncidentRayOrigin.Y;                       
                                if(HitPanelCount == 0)
                                {
                                    SamplePanelID[(j * SampleNum + l) *(Resolution.X * SampleNum * 2) + 2 * (i * SampleNum + k)] = m;
                                    SamplePanelCoord[(j * SampleNum + l) *(Resolution.X * SampleNum * 2 * 2) + 2 * (2 * (i * SampleNum + k) + 0) + 0] = int(IncidentRayOrigin.X);
                                    SamplePanelCoord[(j * SampleNum + l) *(Resolution.X * SampleNum * 2 * 2) + 2 * (2 * (i * SampleNum + k) + 0) + 1] = int(IncidentRayOrigin.Y);
                                    
                                    SamplePanelID[(j * SampleNum + l) *(Resolution.X * SampleNum * 2) + 2 * (i * SampleNum + k) + 1] = m;
                                    SamplePanelCoord[(j * SampleNum + l) *(Resolution.X * SampleNum * 2 * 2) + 2 * (2 * (i * SampleNum + k) + 1) + 0] = int(IncidentRayOrigin.X);
                                    SamplePanelCoord[(j * SampleNum + l) *(Resolution.X * SampleNum * 2 * 2) + 2 * (2 * (i * SampleNum + k) + 1) + 1] = int(IncidentRayOrigin.Y);
                                }else
                                {
                                    SamplePanelID[(j * SampleNum + l) *(Resolution.X * SampleNum * 2) + 2 * (i * SampleNum + k) + 1] = m;
                                    SamplePanelCoord[(j * SampleNum + l) *(Resolution.X * SampleNum * 2 * 2) + 2 * (2 * (i * SampleNum + k) + 1) + 0] = int(IncidentRayOrigin.X);
                                    SamplePanelCoord[(j * SampleNum + l) *(Resolution.X * SampleNum * 2 * 2) + 2 * (2 * (i * SampleNum + k) + 1) + 1] = int(IncidentRayOrigin.Y);
                                }
                                PixelCountPanel[m]++;
                                SampleCountPanel[m]++;
                                HitPanelCount++;
                                //break;
                            }
                        }
                        int SampleSampleCountPanelTotal = 0;
                        for (int a = 0 ; a < SampleCountPanel.Num() ; a++)
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
            if(maxpanel > 0)
            {
                int other = GetTotalValue(PixelCountPanel) - maxpanel;
                if (other > 0)
                {
                    //UE_LOG(LogTemp, Error, TEXT("Sample point on other panel num %d , pixel %d,%d"), other, i, j);
                }
                //SamplePanelBrightness[j * Resolution.X + i] = other * 0.0001;
            }
             if((i == 189 && j == 890)||(i == 190 && j == 889))
             {
                 UE_LOG(LogTemp, Error, TEXT("pixel %d,%d"), i, j);
                 for (int k = 0; k < SampleNum; k++)
                 {
                     for (int l = 0; l < SampleNum; l++) 
                     {

                         UE_LOG(LogTemp, Error, TEXT("Sample %d,%d , hit point panel 1 on %d , pos(%d, %d) , hit point 2 on panel %d , pos(%d, %d)"), k, l,
                             SamplePanelID[(j * SampleNum + l) *(Resolution.X * SampleNum * 2) + 2 * (i * SampleNum + k)],
                         SamplePanelCoord[(j * SampleNum + l) *(Resolution.X * SampleNum * 2 * 2) + 2 * (2 * (i * SampleNum + k) + 0) + 0],
                         SamplePanelCoord[(j * SampleNum + l) *(Resolution.X * SampleNum * 2 * 2) + 2 * (2 * (i * SampleNum + k) + 0) + 1],
                         SamplePanelID[(j * SampleNum + l) *(Resolution.X * SampleNum * 2) + 2 * (i * SampleNum + k) + 1],
                         SamplePanelCoord[(j * SampleNum + l) *(Resolution.X * SampleNum * 2 * 2) + 2 * (2 * (i * SampleNum + k) + 1) + 0],
                         SamplePanelCoord[(j * SampleNum + l) *(Resolution.X * SampleNum * 2 * 2) + 2 * (2 * (i * SampleNum + k) + 1) + 1]);
                     }
                 }
             }
        }
    }
}

//void UShadertestRendering::SetProjectionModel(int ProjectionModel)
//{
//    //this->ProjectionModel = ProjectionModel;
//}

bool UNewShaderRendering::IsSampleInCircle(float i, float j , FIntPoint Resolution)
{
    FVector2D SamplePoint(i, j);

    FVector2D ImageCenter(Resolution.X / 2, Resolution.Y / 2);
    float Dist = (ImageCenter - SamplePoint).Size();
    if (Dist <= ImageCenter.X)
    {
        return true;
    }
    else
    {
        return false;
    }
}

FVector UNewShaderRendering::RayPlaneIntersection(const FVector& RayOrigin, const FVector& RayDirection, const FPlane& Plane, bool& WillIntersection)
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

bool UNewShaderRendering::IsPointInCube(FVector Point)
{
    if ((FMath::Abs(Point.X) >= 0.0) && (Point.X <= 1.0)
        && (Point.Y >= -1.0) && (Point.Y <= 1.0)
        && (Point.Z >= -1.0) && (Point.Z <= 1.0))
    {
        return true;
    }
    return false;
}

FVector2D UNewShaderRendering::LoclSpace2Panel(int PanelID, FVector IntersectPoint, float Radius)
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