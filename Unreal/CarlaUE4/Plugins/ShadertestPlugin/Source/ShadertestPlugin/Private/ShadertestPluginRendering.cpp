#include "ShadertestPluginRendering.h"
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
IMPLEMENT_SHADER_TYPE(, FNewMyComputeShader, TEXT("/Plugin/ShadertestPlugin/Private/TexturePacker.usf"), TEXT("MainCS"), SF_Compute)

void UShadertestRendering::UseComputeShaderArray_RenderThread(
    FRHICommandListImmediate& RHICmdList,
    TArray<FTextureRenderTargetResource*> InTextureRenderTargetResource,
    FTextureRenderTargetResource* OutTextureRenderTargetResource,
    FIntPoint Resolution,
    int SampleNum,
    int ProjectionModel)
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
            static int OldProjectionModel = -1;
            if (Count == 0)
            {
                //UE_LOG(LogTemp, Error, TEXT("if(Count == 0)"));
                PixelInCircle->Init(0, SizeX * SizeY);
                SamplePanelID->Init(15, SizeX * SizeY * SampleNum );
                SamplePanelCoord->Init(0, SizeX * SizeY * SampleNum * SampleNum * 2 * 2);
                SamplePanelBrightness->Init(0.0, SizeX * SizeY);
                //UE_LOG(LogTemp, Error, TEXT("in if before SizeX %d ,SizeY %d, SampleNum %d, SamplePanelID %d, SamplePanelCoord %d"),SizeX, SizeY, SampleNum, SamplePanelID->Num(), SamplePanelCoord->Num());
                CalPixelsRelationship(*PixelInCircle, *SamplePanelID, *SamplePanelCoord, *SamplePanelBrightness, Resolution, SampleNum, ProjectionModel);
                //UE_LOG(LogTemp, Error, TEXT("in if after SizeX %d ,SizeY %d, SampleNum %d, SamplePanelID %d, SamplePanelCoord %d"),SizeX, SizeY, SampleNum, SamplePanelID->Num(), SamplePanelCoord->Num());

                CreateInfoPixelInCircle.ResourceArray = PixelInCircle;
                PixelInCircleBuffer = RHICreateStructuredBuffer(sizeof(int), sizeof(int) * PixelInCircle->Num(),
                    BUF_Static | BUF_ShaderResource, CreateInfoPixelInCircle);
                PixelInCircleSRV = RHICreateShaderResourceView(PixelInCircleBuffer);

                CreateInfoSamplePanelID.ResourceArray = SamplePanelID;
                SamplePanelIDBuffer = RHICreateStructuredBuffer(sizeof(int), sizeof(int) * SamplePanelID->Num(),
                    BUF_Static | BUF_ShaderResource, CreateInfoSamplePanelID);
                SamplePanelIDSRV = RHICreateShaderResourceView(SamplePanelIDBuffer);

                CreateInfoSamplePanelCoord.ResourceArray = SamplePanelCoord;
                SamplePanelCoordBuffer = RHICreateStructuredBuffer(sizeof(int), sizeof(int) * SamplePanelCoord->Num(),
                    BUF_Static | BUF_ShaderResource, CreateInfoSamplePanelCoord);
                SamplePanelCoordSRV = RHICreateShaderResourceView(SamplePanelCoordBuffer);

                CreateInfoSamplePanelBrightness.ResourceArray = SamplePanelBrightness;
                SamplePanelBrightnessBuffer = RHICreateStructuredBuffer(sizeof(float), sizeof(float) * SamplePanelBrightness->Num(),
                    BUF_Static | BUF_ShaderResource, CreateInfoSamplePanelBrightness);
                SamplePanelBrightnessSRV = RHICreateShaderResourceView(SamplePanelBrightnessBuffer);
                OldProjectionModel = ProjectionModel;
            }
            Count++;
            if(OldProjectionModel != ProjectionModel)
            {
                UE_LOG(LogTemp, Error, TEXT("if(OldProjectionModel != ProjectionModel)"));
                CalPixelsRelationship(*PixelInCircle, *SamplePanelID, *SamplePanelCoord, *SamplePanelBrightness, Resolution, SampleNum, ProjectionModel);
;
                CreateInfoPixelInCircle.ResourceArray = PixelInCircle;
                PixelInCircleBuffer = RHICreateStructuredBuffer(sizeof(int), sizeof(int) * PixelInCircle->Num(),
                    BUF_Static | BUF_ShaderResource, CreateInfoPixelInCircle);
                PixelInCircleSRV = RHICreateShaderResourceView(PixelInCircleBuffer);

                CreateInfoSamplePanelID.ResourceArray = SamplePanelID;
                SamplePanelIDBuffer = RHICreateStructuredBuffer(sizeof(int), sizeof(int) * SamplePanelID->Num(),
                    BUF_Static | BUF_ShaderResource, CreateInfoSamplePanelID);
                SamplePanelIDSRV = RHICreateShaderResourceView(SamplePanelIDBuffer);

                CreateInfoSamplePanelCoord.ResourceArray = SamplePanelCoord;
                SamplePanelCoordBuffer = RHICreateStructuredBuffer(sizeof(int), sizeof(int) * SamplePanelCoord->Num(),
                    BUF_Static | BUF_ShaderResource, CreateInfoSamplePanelCoord);
                SamplePanelCoordSRV = RHICreateShaderResourceView(SamplePanelCoordBuffer);

                CreateInfoSamplePanelBrightness.ResourceArray = SamplePanelBrightness;
                SamplePanelBrightnessBuffer = RHICreateStructuredBuffer(sizeof(float), sizeof(float) * SamplePanelBrightness->Num(),
                    BUF_Static | BUF_ShaderResource, CreateInfoSamplePanelBrightness);
                SamplePanelBrightnessSRV = RHICreateShaderResourceView(SamplePanelBrightnessBuffer);
                OldProjectionModel = ProjectionModel;
            }

            //UE_LOG(LogTemp, Error, TEXT("in if out SizeX %d ,SizeY %d, SampleNum %d, SamplePanelID %d, SamplePanelCoord %d"),SizeX, SizeY, SampleNum, SamplePanelID->Num(), SamplePanelCoord->Num());
            RHICmdList.SetComputeShader(ComputeShader->GetComputeShader());

            FSamplerStateRHIRef SamplerState = TStaticSamplerState<SF_Bilinear>::GetRHI();
            // 将参数传递给ComputeShader
            //这里我们实际上能用到的是UAV,追查到SetTexture函数我们可以发现，对于ComputeShader，第二个参数实际上是没有用的
            ComputeShader->SetParameters(RHICmdList, NewInRenderTargetTexture,
                NewOutRenderTargetTexture2, TextureUAV, SamplerState, 
                PixelInCircleSRV, SamplePanelIDSRV, SamplePanelCoordSRV, SamplePanelBrightnessSRV);

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

void UShadertestRendering::UseComputeShaderArray(
    TArray<UTextureRenderTarget2D*> InputRenderTarget,
    class UTextureRenderTarget2D* OutputRenderTarget,
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
                SampleNum,
                ProjectionModel
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

void Fourint8Toint(int& res, int index, int input) {
    // 确保输入值在 0 到 15 的范围内
    check(input >= 0 || input <= 15 || index >= 0 || index <= 7);
    // 清除目标位置上的4位
    res &= ~(0xF << (index * 4));
    // 将 input 移动到目标位置，并使用按位或操作放入 res
    res |= (input << (index * 4));
}

void UShadertestRendering::CalPixelsRelationship(
    TResourceArray<int>& PixelInCircle,
    TResourceArray<int>& SamplePanelID,
    TResourceArray<int>& SamplePanelCoord,
    TResourceArray<float>& SamplePanelBrightness,
    FIntPoint Resolution,
    int SampleNum,
    int ProjectionModel)
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
     TResourceArray<int> SamplePanelIDtmp = SamplePanelID;
     TResourceArray<int> SamplePanelCoordtmp = SamplePanelCoord;

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
                                //linear and sample 2
                                // if(HitPanelCount == 0)
                                // {
                                //     SamplePanelID[(j * Resolution.X + i) * SampleNum * SampleNum * 2 + 2 * (k * SampleNum + l) + 0] = m;
                                //     SamplePanelCoord[(j * Resolution.X + i) * SampleNum * SampleNum * 4 + 4 * (k * SampleNum + l) + 0] = int(IncidentRayOrigin.X);
                                //     SamplePanelCoord[(j * Resolution.X + i) * SampleNum * SampleNum * 4 + 4 * (k * SampleNum + l) + 1] = int(IncidentRayOrigin.Y);

                                //     SamplePanelID[(j * Resolution.X + i) * SampleNum * SampleNum * 2 + 2 * (k * SampleNum + l) + 1] = m;
                                //     SamplePanelCoord[(j * Resolution.X + i) * SampleNum * SampleNum * 4 + 4 * (k * SampleNum + l) + 2] = int(IncidentRayOrigin.X);
                                //     SamplePanelCoord[(j * Resolution.X + i) * SampleNum * SampleNum * 4 + 4 * (k * SampleNum + l) + 3] = int(IncidentRayOrigin.Y);
                                // }
                                // else
                                // {
                                //     SamplePanelID[(j * Resolution.X + i) * SampleNum * SampleNum * 2 + 2 * (k * SampleNum + l) + 1] = m;
                                //     SamplePanelCoord[(j * Resolution.X + i) * SampleNum * SampleNum * 4 + 4 * (k * SampleNum + l) + 2] = int(IncidentRayOrigin.X);
                                //     SamplePanelCoord[(j * Resolution.X + i) * SampleNum * SampleNum * 4 + 4 * (k * SampleNum + l) + 3] = int(IncidentRayOrigin.Y);
                                // }
                                ////quad and sample 2
                                if (HitPanelCount == 0)
                                {
                                   //SamplePanelID[((j * SampleNum + l) *(Resolution.X * SampleNum) + (i * SampleNum + k)) * 2] = m;
                                   Fourint8Toint(SamplePanelID[(j * SampleNum + l) * Resolution.X + i],2*k,m);
                                   SamplePanelCoord[((j * SampleNum + l) *(Resolution.X * SampleNum) + (i * SampleNum + k)) * 4 + 0] = int(IncidentRayOrigin.X);
                                   SamplePanelCoord[((j * SampleNum + l) *(Resolution.X * SampleNum) + (i * SampleNum + k)) * 4 + 1] = int(IncidentRayOrigin.Y);

                                   //SamplePanelID[((j * SampleNum + l) *(Resolution.X * SampleNum) + (i * SampleNum + k)) * 2 + 1] = m;
                                   Fourint8Toint(SamplePanelID[(j * SampleNum + l) * Resolution.X + i], 2 * k + 1, m);
                                   SamplePanelCoord[((j * SampleNum + l) *(Resolution.X * SampleNum) + (i * SampleNum + k)) * 4 + 2] = int(IncidentRayOrigin.X);
                                   SamplePanelCoord[((j * SampleNum + l) *(Resolution.X * SampleNum) + (i * SampleNum + k)) * 4 + 3] = int(IncidentRayOrigin.Y);
                                }
                                else
                                {
                                   Fourint8Toint(SamplePanelID[(j * SampleNum + l) * Resolution.X + i], 2 * k + 1, m);
                                   SamplePanelCoord[((j * SampleNum + l) *(Resolution.X * SampleNum) + (i * SampleNum + k)) * 4 + 2] = int(IncidentRayOrigin.X);
                                   SamplePanelCoord[((j * SampleNum + l) *(Resolution.X * SampleNum) + (i * SampleNum + k)) * 4 + 3] = int(IncidentRayOrigin.Y);
                                }
                                //SamplePanelID[((j * SampleNum + l) *(Resolution.X * SampleNum) + (i * SampleNum + k))] = m;
                                //SamplePanelCoord[((j * SampleNum + l) *(Resolution.X * SampleNum) + (i * SampleNum + k)) * 2 + 0] = int(IncidentRayOrigin.X);
                                //SamplePanelCoord[((j * SampleNum + l) *(Resolution.X * SampleNum) + (i * SampleNum + k)) * 2 + 1] = int(IncidentRayOrigin.Y);
                                //quad
                                //SamplePanelID[((j * SampleNum + l) *(Resolution.X * SampleNum) + (i * SampleNum + k))] = m;
                                //SamplePanelCoord[((j * SampleNum + l) *(Resolution.X * SampleNum) + (i * SampleNum + k)) * 2 + 0] = int(IncidentRayOrigin.X);
                                //SamplePanelCoord[((j * SampleNum + l) *(Resolution.X * SampleNum) + (i * SampleNum + k)) * 2 + 1] = int(IncidentRayOrigin.Y);
                                //linear
                                // SamplePanelID[(j * Resolution.X + i) * SampleNum * SampleNum + k * SampleNum + l] = m;
                                // SamplePanelCoord[(j * Resolution.X + i) * SampleNum * SampleNum * 2 + 2 * (k * SampleNum + l) + 0] = int(IncidentRayOrigin.X);
                                // SamplePanelCoord[(j * Resolution.X + i) * SampleNum * SampleNum * 2 + 2 * (k * SampleNum + l) + 1] = int(IncidentRayOrigin.Y);
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

    uint32 NewPixelIndex = 0;
    uint32 SampleIDIndex = 0;
    uint32 SampleCoordIndex = 0;
    //j,i是每一个block最左上角的pixel的图像坐标
    //for (int j = 0; j < Resolution.Y; j += NUM_THREADS_PER_GROUP_DIMENSION)
    //{
    //    for (int i = 0; i < Resolution.X; i += NUM_THREADS_PER_GROUP_DIMENSION)
    //    {
    //        //blockj,blocki是pixel在block内部的坐标
    //        //j+blockj , i+blocki是当前pixel的图像坐标
    //        for (int blockj = 0; (blockj < NUM_THREADS_PER_GROUP_DIMENSION) && (blockj + j < Resolution.Y); blockj++)
    //        {
    //            for (int blocki = 0; (blocki < NUM_THREADS_PER_GROUP_DIMENSION) && (blocki + i < Resolution.X); blocki++)
    //            {
    //                uint32 OldPixelIndex = (j + blockj)*Resolution.X + i + blocki;
    //                int NewPixelj = NewPixelIndex / Resolution.X;
    //                int NewPixeli = NewPixelIndex % Resolution.X;
    //                for(int k = 0; k < SampleNum; k++)
    //                {
    //                    for(int l = 0; l < SampleNum; l++)
    //                    {
    //                        int tmpsampleindex = ((j + blockj) * SampleNum + l) * (Resolution.X * SampleNum) + ((i + blocki) * SampleNum + k);
    //                        int sampleindex = (NewPixelj * SampleNum + l) * (Resolution.X * SampleNum) + (NewPixeli * SampleNum + k);
    //                        SamplePanelID[sampleindex * 2 + 0] = SamplePanelIDtmp[tmpsampleindex * 2 + 0];
    //                        SamplePanelID[sampleindex * 2 + 1] = SamplePanelIDtmp[tmpsampleindex * 2 + 1];

    //                        SamplePanelCoord[sampleindex * 4 + 0] = SamplePanelCoordtmp[tmpsampleindex * 4 + 0];
    //                        SamplePanelCoord[sampleindex * 4 + 1] = SamplePanelCoordtmp[tmpsampleindex * 4 + 1];
    //                        SamplePanelCoord[sampleindex * 4 + 2] = SamplePanelCoordtmp[tmpsampleindex * 4 + 2];
    //                        SamplePanelCoord[sampleindex * 4 + 3] = SamplePanelCoordtmp[tmpsampleindex * 4 + 3];
    //                    }
    //                }
    //                NewPixelIndex++;
    //            }
    //        }
    //    }
    //}
    //for(int j = 0; j < Resolution.Y; j++)
    //{
    //    for(int i = 0; i < Resolution.X; i++)
    //    {
    //        SampleIndexLookup[j * Resolution.X + i] = CalculateBuffIndex(i, j, NUM_THREADS_PER_GROUP_DIMENSION, Resolution.X, Resolution.Y);
    //    }
    //}
}

//void UShadertestRendering::SetProjectionModel(int ProjectionModel)
//{
//    //this->ProjectionModel = ProjectionModel;
//}

bool UShadertestRendering::IsSampleInCircle(float i, float j, FIntPoint Resolution)
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