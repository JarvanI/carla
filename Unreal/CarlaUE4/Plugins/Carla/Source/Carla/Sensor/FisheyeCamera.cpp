// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma de Barcelona (UAB). This work is licensed under the terms of the MIT license. For a copy, see <https://opensource.org/licenses/MIT>.



#include "Carla.h"
#include "FisheyeCamera.h"

#include "Carla/Game/CarlaStatics.h"

#include "Components/DrawFrustumComponent.h"
#include "Engine/Classes/Engine/Scene.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HighResScreenshot.h"
#include "ContentStreaming.h"
#include "FileHelper.h"
#include "IImageWrapper.h"
#include "ImageUtils.h"
#include "IImageWrapperModule.h"
#include "ModuleManager.h"
#include "ParallelFor.h"
#include "Actor/ActorBlueprintFunctionLibrary.h"
// #include "Actor/ActorBlueprintFunctionLibrary.h"

static auto FISHEYE_COUNTER = 0u;

// =============================================================================
// -- Local static methods -----------------------------------------------------
// =============================================================================

// Local namespace to avoid name collisions on unit builds.
namespace FisheyeCamera_local_ns {

    static void SetCameraDefaultOverrides(USceneCaptureComponent2D &CaptureComponent2D);

    static void ConfigureShowFlags(FEngineShowFlags &ShowFlags, bool bPostProcessing = true);

    static auto GetQualitySettings(UWorld *World)
    {
        auto Settings = UCarlaStatics::GetCarlaSettings(World);
        check(Settings != nullptr);
        return Settings->GetQualityLevel();
    }
} // namespace FisheyeCamera_local_ns

// =============================================================================
// -- AFisheyeCamera ------------------------------------------------------
// =============================================================================

AFisheyeCamera::AFisheyeCamera(const FObjectInitializer &ObjectInitializer)
    : Super(ObjectInitializer)
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PrePhysics;

    for (int i = 0; i < 5; i++)
    {
        CaptureRenderTarget[i] = CreateDefaultSubobject<UTextureRenderTarget2D>(
            FName(*FString::Printf(TEXT("FisheyeCaptureRenderTarget_%d"), i)));
        CaptureRenderTarget[i]->CompressionSettings = TextureCompressionSettings::TC_Default;
        //CaptureRenderTarget[i]->MipGenSettings = TMGS_NoMipmaps;
        CaptureRenderTarget[i]->SRGB = false;
        CaptureRenderTarget[i]->bAutoGenerateMips = false;
        CaptureRenderTarget[i]->AddressX = TextureAddress::TA_Clamp;
        CaptureRenderTarget[i]->AddressY = TextureAddress::TA_Clamp;
        CaptureRenderTarget[i]->SizeX = ImageWidth;
        CaptureRenderTarget[i]->SizeY = ImageWidth;
        //RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent"));
        CaptureComponent2D[i] = CreateDefaultSubobject<USceneCaptureComponent2D>(
            FName(*FString::Printf(TEXT("FisheyeSceneCaptureComponent2D_%d"), i)));
        CaptureComponent2D[i]->FOVAngle = 90;
        CaptureComponent2D[i]->SetupAttachment(RootComponent);
        //CaptureComponent2D[i]->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
    }
    //Front
    CaptureComponent2D[0]->SetRelativeRotation(FRotator(0, 0, 0));
    //Left
    CaptureComponent2D[1]->SetRelativeRotation(FRotator(0, -90, 0));
    //Right
    CaptureComponent2D[2]->SetRelativeRotation(FRotator(0, 90, 0));
    //Top
    CaptureComponent2D[3]->SetRelativeRotation(FRotator(90, 0, 0));
    //Bottom
    CaptureComponent2D[4]->SetRelativeRotation(FRotator(-90, 0, 0));

    PlaneArray.Add(FPlane(1, 0, 0, 1));
    PlaneArray.Add(FPlane(0, 1, 0, -1));
    PlaneArray.Add(FPlane(0, 1, 0, 1));
    PlaneArray.Add(FPlane(0, 0, 1, 1));
    PlaneArray.Add(FPlane(0, 0, 1, -1));

    FishEyeTexture = CreateDefaultSubobject<UTextureRenderTarget2D>(
        FName(*FString::Printf(TEXT("FishEyeTexture"))));
    FishEyeTexture->CompressionSettings = TextureCompressionSettings::TC_VectorDisplacementmap;
    //FishEyeTexture->MipGenSettings = TMGS_NoMipmaps;
    FishEyeTexture->SRGB = false;
    FishEyeTexture->bAutoGenerateMips = false;
    FishEyeTexture->AddressX = TextureAddress::TA_Clamp;
    FishEyeTexture->AddressY = TextureAddress::TA_Clamp;
    FishEyeTexture->SizeX = ImageWidth;
    FishEyeTexture->SizeY = ImageWidth;

    Radius = float(ImageWidth) / 2;
    SampleDist = 1.0 / (2.0 * float(SampleNum));

    UE_LOG(LogTemp, Warning, TEXT("Test  AFishEye::AFishEye() "));
    UE_LOG(LogTemp, Warning, TEXT("SampleNume : %d , SampleDist : %lf"), SampleNum, SampleDist);

    for (int i = 0; i < 5; i++)
    {
        FisheyeCamera_local_ns::SetCameraDefaultOverrides(*CaptureComponent2D[i]);
    }
    ++FISHEYE_COUNTER;
}

void AFisheyeCamera::BeginPlay()
{
    using namespace FisheyeCamera_local_ns;

    // Setup render target.

    // Determine the gamma of the player.

    const bool bInForceLinearGamma = !bEnablePostProcessingEffects;

    for (int i = 0; i < 5; ++i) {
        CaptureRenderTarget[i]->InitCustomFormat(ImageWidth, ImageWidth, PF_B8G8R8A8, bInForceLinearGamma);
        if (bEnablePostProcessingEffects)
        {
            CaptureRenderTarget[i]->TargetGamma = TargetGamma;
        }
        check(IsValid(CaptureComponent2D[i]) && !CaptureComponent2D[i]->IsPendingKill());
        CaptureComponent2D[i]->Deactivate();
        CaptureComponent2D[i]->TextureTarget = CaptureRenderTarget[i];

        //SetUpSceneCaptureComponent(*CaptureComponent2D[i]);

        CaptureComponent2D[i]->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
        CaptureComponent2D[i]->UpdateContent();
        CaptureComponent2D[i]->Activate();

        FisheyeCamera_local_ns::ConfigureShowFlags(CaptureComponent2D[i]->ShowFlags,
            bEnablePostProcessingEffects);
    }

    // Make sure that there is enough time in the render queue.
    UKismetSystemLibrary::ExecuteConsoleCommand(
        GetWorld(),
        FString("g.TimeoutForBlockOnRenderFence 300000"));

    // This ensures the camera is always spawning the rain drops in case the
    // weather was previously set to has rain
    GetEpisode().GetWeather()->NotifyWeather();
    auto t1 = std::chrono::system_clock::now();
    //UE_LOG(LogTemp, Warning, TEXT("before ImageWidth %d , SSAA %d, SampleDist %lf"), ImageWidth, SampleNum, SampleDist);
    //UE_LOG(LogTemp, Warning, TEXT("ImagingPixels before %d "), ImagingPixels.Num());
    //UE_LOG(LogTemp, Warning, TEXT("ImaginPixelsQuerry before %d "), ImaginPixelsQuerry.Num());
    CalPixelsRelationship();
    //UE_LOG(LogTemp, Warning, TEXT("after ImageWidth %d , SSAA %d, SampleDist %lf"), ImageWidth, SampleNum, SampleDist);
    //UE_LOG(LogTemp, Warning, TEXT("ImagingPixels after %d "), ImagingPixels.Num());
    //UE_LOG(LogTemp, Warning, TEXT("ImaginPixelsQuerry after %d "), ImaginPixelsQuerry.Num());
    auto t2 = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;
    UE_LOG(LogTemp, Warning, TEXT("Jarvan CalPixelsRelationshipcost time:%.8lf"), elapsed_seconds_1.count());
    //为啥父类们的BeginPlay()要放在最后才执行 ?
    Super::BeginPlay();
}

void AFisheyeCamera::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    // Add the view information every tick. Its only used for one tick and then
    // removed by the streamer.
    // IStreamingManager::Get().AddViewInformation(
    //     CaptureComponent2D->GetComponentLocation(),
    //     ImageWidth,
    //     ImageWidth / FMath::Tan(CaptureComponent2D->FOVAngle));

    FDateTime Time = FDateTime::Now();
    int64 Timestamp = Time.ToUnixTimestamp();
    FString TimestampStr = FString::FromInt(Timestamp);

    //保存5个2D图片
    //for (int i = 0; i < 5; i++)
    //{
    //    FString SaveFileName = FPaths::ProjectSavedDir();

    //    SaveFileName.Append(FString("FishEyeSplitNO"));
    //    SaveFileName.Append(FString::FromInt(i));
    //    SaveFileName.Append(TimestampStr);
    //    SaveFileName.Append(".jpg");
    //    ScreenshotToImage2D(SaveFileName, i);
    //}
    //FString SaveFileName = FPaths::ProjectSavedDir();
    //SaveFileName.Append(FString("FishEye"));
    //SaveFileName.Append(TimestampStr);
    //SaveFileName.Append(".jpg");
    //GetFishEyePic(SaveFileName);
    //UE_LOG(LogTemp, Warning, TEXT("ImageWidth %d , SSAA %d, SampleDist %lf"), ImageWidth, SampleNum, SampleDist);

    SendFisheyePixelsInRenderThread(*this);
}

void AFisheyeCamera::GetFishEyePic(const FString& InImagePath)
{
    auto start = FDateTime::Now().GetTimeOfDay().GetTotalMilliseconds();
    check(FishEyeTexture != nullptr);

    OutDataFishEye.Init(FColor(0, 0, 0), ImageWidth*ImageWidth);
    int32 Width = CaptureComponent2D[0]->TextureTarget->SizeX;
    int32 Height = CaptureComponent2D[0]->TextureTarget->SizeY;
    FTextureRenderTargetResource* TextureRenderTargetResourceFront = CaptureComponent2D[0]->TextureTarget->GameThread_GetRenderTargetResource();
    FTextureRenderTargetResource* TextureRenderTargetResourceLeft = CaptureComponent2D[1]->TextureTarget->GameThread_GetRenderTargetResource();
    FTextureRenderTargetResource* TextureRenderTargetResourceRight = CaptureComponent2D[2]->TextureTarget->GameThread_GetRenderTargetResource();
    FTextureRenderTargetResource* TextureRenderTargetResourceTop = CaptureComponent2D[3]->TextureTarget->GameThread_GetRenderTargetResource();
    FTextureRenderTargetResource* TextureRenderTargetResourceBottom = CaptureComponent2D[4]->TextureTarget->GameThread_GetRenderTargetResource();

    TextureRenderTargetResourceFront->ReadPixels(OutDataFront, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), FIntRect(0, 0, Width, Height));
    TextureRenderTargetResourceLeft->ReadPixels(OutDataLeft, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), FIntRect(0, 0, Width, Height));
    TextureRenderTargetResourceRight->ReadPixels(OutDataRight, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), FIntRect(0, 0, Width, Height));
    TextureRenderTargetResourceTop->ReadPixels(OutDataTop, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), FIntRect(0, 0, Width, Height));
    TextureRenderTargetResourceBottom->ReadPixels(OutDataBottom, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), FIntRect(0, 0, Width, Height));

    auto mid = FDateTime::Now().GetTimeOfDay().GetTotalMilliseconds();
    for(int i=0;i< ImaginPixelsQuerry.Num();i++)
    {
        ImagingPixel ImagingPixel = ImagingPixels[ImaginPixelsQuerry[i]];
        TArray<FColor> PixelColor;
        TArray<FColor> *OriginPtr;
        for (int k = 0; k < SampleNum; k++)
        {
            for (int l = 0; l < SampleNum; l++)
            {
                int SampleID = k * SampleNum + l;
                if (ImagingPixel.SampleInImage[SampleID] == 0)
                {
                    PixelColor.Add(FColor(0, 0, 0));
                }
                else
                {
                    auto& SampleOrigin = ImagingPixel.SampleInfosArray[SampleID].ISampleOriginPanelImageSpace;
                    TArray<FColor> SampleColor;
                    check(SampleOrigin.Num() > 0);
                    for (int m = 0; m < SampleOrigin.Num(); m++)
                    {
                        int x = SampleOrigin[m].TexelPos.i;
                        int y = SampleOrigin[m].TexelPos.j;
                        OriginPtr = GetOutData(SampleOrigin[m].ID);
                        SampleColor.Add((*OriginPtr)[x * ImageWidth + y]);
                    }
                    PixelColor.Add(CalAvgColor(SampleColor));
                }
            }
        }
        OutDataFishEye[ImagingPixel.i * ImageWidth + ImagingPixel.j] = CalAvgColor(PixelColor);
    }
    //ParallelFor(ImaginPixelsQuerry.Num(), [&](int i)
    //{
    //    auto& ImagingPixel = ImagingPixels[ImaginPixelsQuerry[i]];
    //    TArray<FColor> PixelColor;
    //    TArray<FColor> *OriginPtr;
    //    for (int k = 0; k < SampleNum; k++)
    //    {
    //        for (int l = 0; l < SampleNum; l++)
    //        {
    //            int SampleID = k * SampleNum + l;
    //            if (ImagingPixel.SampleInImage[SampleID] == 0)
    //            {
    //                PixelColor.Add(FColor(0, 0, 0));
    //            }
    //            else
    //            {
    //                auto& SampleOrigin = ImagingPixel.SampleInfosArray[SampleID].ISampleOriginPanelImageSpace;
    //                TArray<FColor> SampleColor;
    //                check(SampleOrigin.Num() > 0);
    //                for (int m = 0; m < SampleOrigin.Num(); m++)
    //                {
    //                    int x = SampleOrigin[m].TexelPos.i;
    //                    int y = SampleOrigin[m].TexelPos.j;
    //                    OriginPtr = GetOutData(SampleOrigin[m].ID);
    //                    SampleColor.Add((*OriginPtr)[x * ImageWidth + y]);
    //                }
    //                PixelColor.Add(CalAvgColor(SampleColor));
    //            }
    //        }
    //    }
    //    OutDataFishEye[ImagingPixel.i * ImageWidth + ImagingPixel.j] = CalAvgColor(PixelColor);
    //});
    auto end = FDateTime::Now().GetTimeOfDay().GetTotalMilliseconds();

    UE_LOG(LogTemp, Warning, TEXT("mid -start : %f, end-mid %f"), mid - start, end - mid);

    ColorToImage(InImagePath, OutDataFishEye, Width, Height);
}

FColor AFisheyeCamera::CalAvgColor(TArray<FColor>& PixelColor)
{
    int R = 0;
    int G = 0;
    int B = 0;
    int A = 0;
    for (int i = 0; i < PixelColor.Num(); i++)
    {
        R += PixelColor[i].R;
        G += PixelColor[i].G;
        B += PixelColor[i].B;
        A += PixelColor[i].A;
    }
    R /= PixelColor.Num();
    G /= PixelColor.Num();
    B /= PixelColor.Num();
    A /= PixelColor.Num();

    return FColor(uint8(R), uint8(G), uint8(B), uint8(A));
}

TArray<FColor>* AFisheyeCamera::GetOutData(int CameraID)
{
    switch (CameraID)
    {
    case 0:
        return &OutDataFront;
        break;
    case 1:
        return &OutDataLeft;
        break;
    case 2:
        return &OutDataRight;
        break;
    case 3:
        return &OutDataTop;
        break;
    case 4:
        return &OutDataBottom;
        break;
    }
    return &OutDataFront;
}

void AFisheyeCamera::ScreenshotToImage2D(const FString& InImagePath, int i)
{

    if (CaptureComponent2D[i] && CaptureComponent2D[i]->TextureTarget)
    {
        //GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, FString::FromInt(CaptureComponent2D[i]->TextureTarget->SizeX));
        //GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, FString::FromInt(CaptureComponent2D[i]->TextureTarget->SizeY));

        FTextureRenderTargetResource* TextureRenderTargetResource = CaptureComponent2D[i]->TextureTarget->GameThread_GetRenderTargetResource();
        int32 Width = CaptureComponent2D[i]->TextureTarget->SizeX;
        int32 Height = CaptureComponent2D[i]->TextureTarget->SizeY;
        //FIntRect Pos = CaptureComponent2DMulti->CameraAttributeMap[ID].PosInRenderTarget;
        //uint32 Width = Pos.Max.X - Pos.Min.X;
        //uint32 Height = Pos.Max.Y - Pos.Min.Y;
        if (Width > 0 && Height > 0) {
            TArray<FColor> OutData;
            TextureRenderTargetResource->ReadPixels(OutData, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), FIntRect(0, 0, Width, Height));
            //UE_LOG(LogTemp, Warning, TEXT("ID is %d , pos in render target is %d "), OutData.Num(), OutData.GetAllocatedSize()); 
            //CaptureComponent2DMulti->CameraAttributeMap[ID].PosInRenderTarget
            //UE_LOG(LogTemp, Warning, TEXT("after readpixels , TArray size is %d , data size is %d "), OutData.Num(), OutData.GetAllocatedSize());
            ColorToImage(InImagePath, OutData, Width, Height);
            //UE_LOG(LogTemp, Warning, TEXT("width = %d , height = %d , TArray size is %d"), Width, Height, OutData.Num());
        }
    }
    else {
        UE_LOG(LogTemp, Warning, TEXT("NO CaptureComponent2D_No.%d->TextureTarget"), i);
    }
}
void AFisheyeCamera::ColorToImage(const FString& InImagePath, TArray<FColor> InColor, int32 InWidth, int32 InHeight)
{
    IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked <IImageWrapperModule>("ImageWrapper");
    FString Ex = FPaths::GetExtension(InImagePath);

    if (Ex.Equals(TEXT("jpg"), ESearchCase::IgnoreCase) || Ex.Equals(TEXT("jpeg"), ESearchCase::IgnoreCase))
    {
        TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);
        if (ImageWrapper->SetRaw(InColor.GetData(), InColor.GetAllocatedSize(), InWidth, InHeight, ERGBFormat::BGRA, 8))
        {
            FFileHelper::SaveArrayToFile(ImageWrapper->GetCompressed(100), *InImagePath);
        }
    }
    else
    {
        TArray<uint8> OutPNG;
        for (FColor& color : InColor)
        {
            color.A = 255;
        }
        FImageUtils::CompressImageArray(InWidth, InHeight, InColor, OutPNG);
        FFileHelper::SaveArrayToFile(OutPNG, *InImagePath);
    }
}

void AFisheyeCamera::ColorToImage(const FString& InImagePath, TArray<uint8> InColor, int32 InWidth, int32 InHeight)
{
    IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked <IImageWrapperModule>("ImageWrapper");
    FString Ex = FPaths::GetExtension(InImagePath);

    if (Ex.Equals(TEXT("jpg"), ESearchCase::IgnoreCase) || Ex.Equals(TEXT("jpeg"), ESearchCase::IgnoreCase))
    {
        //TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);
        //if (ImageWrapper->SetRaw(InColor, InWidth * InHeight * 4, InWidth, InHeight, ERGBFormat::BGRA, 8))
        //{
        //    FFileHelper::SaveArrayToFile(InColor, *InImagePath);
        //    //FFileHelper::SaveArrayToFile(ImageWrapper->GetCompressed(100), *InImagePath);
        //}
        UE_LOG(LogTemp, Warning, TEXT("djw Ex.Equals(TEXT(jpg)"));

    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("djw void AFisheyeCamera::ColorToImage(const FString& InImagePath, uint8* InColor, int32 InWidth, int32 InHeight)"));
        //TArray<uint8> OutPNG;
        //for (FColor& color : InColor)
        //{
        //    color.A = 255;
        //}
        //FImageUtils::CompressImageArray(InWidth, InHeight, InColor, OutPNG);
        //FFileHelper::SaveArrayToFile(OutPNG, *InImagePath);
    }
    FFileHelper::SaveArrayToFile(InColor, *InImagePath);
}

void AFisheyeCamera::CalPixelsRelationship()
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

    int testIsPixelInCircle = 0;
    int testaddpixelpoint = 0;
    FCriticalSection Mutex;
    //从上到下i, 从左到右j

    //使用多线程加速 ,由于加了Mutex的原因 , 速度反而比单线程慢
    //ParallelFor(ImageWidth, [&](int i)
    //{
    //    for (int j = 0; j < ImageWidth; j++)
    //    {
    //        //sample point
    //        float Samplei;
    //        float Samplej;
    //        int SampleID;
    //        //UE_LOG(LogTemp, Warning, TEXT("PixelInfo : (%d,%d)"), i, j);
    //        for (int k = 0; k < SampleNum; k++)
    //        {
    //            for (int l = 0; l < SampleNum; l++)
    //            {
    //                Samplei = float(i) + SampleDist * (2 * k + 1);
    //                Samplej = float(j) + SampleDist * (2 * l + 1);
    //                if (IsSampleInCircle(Samplei, Samplej))
    //                {
    //                    Mutex.Lock();
    //                    ImagingPixel* CurPixelPtr = ImagingPixels.Find(i * ImageWidth + j);
    //                    if (!CurPixelPtr)
    //                    {
    //                        ImaginPixelsQuerry.Add(i * ImageWidth + j);
    //                        ImagingPixels.Add(i * ImageWidth + j, ImagingPixel(i, j, SampleNum));
    //                        CurPixelPtr = ImagingPixels.Find(i * ImageWidth + j);
    //                    }
    //                    Mutex.Unlock();
    //                    SampleID = k * SampleNum + l;
    //                    CurPixelPtr->SampleInImage[SampleID] = 1;
    //                    //UE_LOG(LogTemp, Warning, TEXT("Sample info : No.%d (%lf,%lf) , in circle %d"), SampleID, Samplei, Samplej, CurPixelPtr->SampleInImage[SampleID]);
    //                    //FVector p(-1, (float(j) + 0.5 - Radius) / Radius, (-(float(i) + 0.5) + Radius) / Radius);
    //                    FVector p(-1, (Samplej - Radius) / Radius, (-Samplei + Radius) / Radius);
    //                    FVector po = o - p;
    //                    FVector pO = O - p;
    //                    //thetad是pO和oO的夹角 , 也就是逆向的出射光线和x轴正向的夹角
    //                    float thetad = FMath::Acos(FVector::DotProduct(oO, pO) / (oO.Size() * pO.Size()));
    //                    //theta是OP和oO轴的夹角 , 也就是逆向的入射光线和x轴正向的夹角
    //                    float theta;
    //                    switch (ProjectionModel)
    //                    {
    //                    case 0:
    //                        //透视投影 (perspective projection)
    //                        theta = thetad;
    //                        break;
    //                    case 1:
    //                        //体视投影 (stereographic projection)
    //                        theta = 2 * FMath::Atan(FMath::Tan(thetad) / 2);
    //                        break;
    //                    case 2:
    //                        //等距投影 (equidistance projection)
    //                        theta = FMath::Tan(thetad);
    //                        break;
    //                    case 3:
    //                        //等积投影(equisolid angle projection)
    //                        theta = 2 * FMath::Asin(FMath::Tan(thetad) / 2);
    //                        break;
    //                    case 4:
    //                        //正交投影 (orthogonal projection)
    //                        theta = FMath::Asin(FMath::Tan(thetad));
    //                        break;
    //                    default:
    //                        theta = thetad;
    //                    }
    //                    //alpha是po和和y轴正向的夹角 , 同样是Op'(p'是P点在yz平面上的投影)和y轴正向的夹角
    //                    FVector ppie(0, p.Y, p.Z);
    //                    float alpha = FMath::Acos(FVector::DotProduct(ppie, YNormal) / (ppie.Size() * YNormal.Size()));
    //                    //上面用反余弦函数求到的角度范围为[0,pi] , 而半球在xy平面的投影(即成像面)的角度是[0,2pi] , 所以需要纠正
    //                    if (ppie.Z < 0)
    //                    {
    //                        alpha = 2 * PI - alpha;
    //                    }
    //                    //现在 , 已知OP和x轴正向角度为theta  , Op'和y轴正向的角度为alpha , 计算出OP的单位向量
    //                    FVector OPNormal(FMath::Cos(theta), FMath::Sin(theta) * FMath::Cos(alpha), FMath::Sin(theta) * FMath::Sin(alpha));
    //                    //UE_LOG(LogTemp, Warning, TEXT("PixelInfo : (%d,%d)"), i, j);
    //                    //UE_LOG(LogTemp, Warning, TEXT("p : (%lf,%lf,%lf)"), p.X, p.Y,p.Z);
    //                    //UE_LOG(LogTemp, Warning, TEXT("po : (%lf,%lf,%lf)"), po.X, po.Y, po.Z);
    //                    //UE_LOG(LogTemp, Warning, TEXT("pO : (%lf,%lf,%lf)"), pO.X, pO.Y, pO.Z);
    //                    //UE_LOG(LogTemp, Warning, TEXT("thetad, theta, alpha : (%lf,%lf,%lf)"), thetad, theta, alpha);
    //                    //UE_LOG(LogTemp, Warning, TEXT("OPNormal : (%lf,%lf,%lf)"), OPNormal.X, OPNormal.Y, OPNormal.Z);
    //                    for (int m = 0; m < PlaneArray.Num(); m++)
    //                    {
    //                        //归一化的空间坐标下的交点
    //                        bool WillIntersect = false;
    //                        FVector IntersectPointNormal = RayPlaneIntersection(FVector::ZeroVector, OPNormal, PlaneArray[m], WillIntersect);
    //                        //FVector IntersectPointNormal = FMath::RayPlaneIntersection(FVector::ZeroVector, OPNormal, PlaneArray[m]);
    //                        if (i == 122 && j == 197 && k == 3 && l == 2)
    //                        {
    //                            UE_LOG(LogTemp, Warning, TEXT("Panel % d , IntersectPointNormal : (%lf,%lf,%lf) , WillIntersect %d , incube %d"),
    //                                m, IntersectPointNormal.X, IntersectPointNormal.Y, IntersectPointNormal.Z, WillIntersect, IsPointInCube(IntersectPointNormal));
    //                        }
    //                        //当找到OP和2D图像的交点
    //                        if (WillIntersect && IsPointInCube(IntersectPointNormal))
    //                        {
    //                            //sample origin和5个panel构成的包围盒只能有1个交点 , 但是这个交点却可能在panel的拼接缝上 , 因此最多可能有3个panel space坐标
    //                            //UE_LOG(LogTemp, Warning, TEXT("IntersectPoint : (%lf,%lf,%lf)"), IntersectPoint.X, IntersectPoint.Y, IntersectPoint.Z);
    //                            CurPixelPtr->SampleInfosArray[SampleID].FSampleOriginPanelLocalSpaceNormal = IntersectPointNormal;
    //                            //局部空间坐标
    //                            FVector IntersectPoint = IntersectPointNormal * Radius;
    //                            //UE_LOG(LogTemp, Warning, TEXT("IntersectPoint : (%lf,%lf,%lf)"), IntersectPoint.X, IntersectPoint.Y, IntersectPoint.Z);
    //                            //连续的屏幕坐标 , 坐标原点在左上角 , 竖直朝下是i(x), 水平朝右是j(y)
    //                            FVector2D IncidentRayOrigin = LoclSpace2Panel(m, IntersectPoint);
    //                            if (int(IncidentRayOrigin.X) >= ImageWidth)
    //                            {
    //                                IncidentRayOrigin.X = float(ImageWidth - 1) + 0.5;
    //                                UE_LOG(LogTemp, Warning, TEXT("Pixel (%d,%d)"), CurPixelPtr->i, CurPixelPtr->j);
    //                                UE_LOG(LogTemp, Warning, TEXT("IntersectPoint : (%lf,%lf,%lf)"), IntersectPoint.X, IntersectPoint.Y, IntersectPoint.Z);
    //                            }
    //                            if (int(IncidentRayOrigin.Y) >= ImageWidth)
    //                            {
    //                                IncidentRayOrigin.Y = float(ImageWidth - 1) + 0.5;
    //                            }
    //                            CurPixelPtr->SampleInfosArray[SampleID].ISampleOriginPanelImageSpace.Add(
    //                                ITexel(m, int(IncidentRayOrigin.X), int(IncidentRayOrigin.Y)));
    //                            if (i == 122 && j == 197 && k == 3 && l == 2)
    //                            {
    //                                UE_LOG(LogTemp, Warning, TEXT("IntersectPoint : (%lf,%lf,%lf)"), IntersectPoint.X, IntersectPoint.Y, IntersectPoint.Z);
    //                                UE_LOG(LogTemp, Warning, TEXT("IntersectRayorigin : (%d,%d)"), int(IncidentRayOrigin.X), int(IncidentRayOrigin.Y));
    //                                UE_LOG(LogTemp, Warning, TEXT("Panel % d , IntersectPointNormal : (%lf,%lf,%lf)"), m, IntersectPointNormal.X, IntersectPointNormal.Y, IntersectPointNormal.Z);
    //                                //UE_LOG(LogTemp, Warning, TEXT("texel panel id : %d , (%d,%d)"), 
    //                                //    CurPixelPtr->ISampleOriginTexelImageSpace[SampleID].ID, 
    //                                //    CurPixelPtr->ISampleOriginTexelImageSpace[SampleID].TexelPos.i, 
    //                                //    CurPixelPtr->ISampleOriginTexelImageSpace[SampleID].TexelPos.j);
    //                            }
    //                            testaddpixelpoint++;
    //                        }
    //                    }
    //                }
    //            }
    //        }
    //    }
    //});

    for (int i = 0; i < ImageWidth; i++)
    {
        for (int j = 0; j < ImageWidth; j++)
        {
            //sample point
            float Samplei;
            float Samplej;
            int SampleID;
            //UE_LOG(LogTemp, Warning, TEXT("PixelInfo : (%d,%d)"), i, j);
            for (int k = 0; k < SampleNum; k++)
            {
                for (int l = 0; l < SampleNum; l++)
                {
                    Samplei = float(i) + SampleDist * (2 * k + 1);
                    Samplej = float(j) + SampleDist * (2 * l + 1);

                    if (IsSampleInCircle(Samplei, Samplej))
                    {

                        ImagingPixel* CurPixelPtr = ImagingPixels.Find(i * ImageWidth + j);
                        if (!CurPixelPtr)
                        {
                            ImaginPixelsQuerry.Add(i * ImageWidth + j);
                            ImagingPixels.Add(i * ImageWidth + j, ImagingPixel(i, j, SampleNum));
                            CurPixelPtr = ImagingPixels.Find(i * ImageWidth + j);
                        }
                        SampleID = k * SampleNum + l;
                        CurPixelPtr->SampleInImage[SampleID] = 1;
                        //UE_LOG(LogTemp, Warning, TEXT("Sample info : No.%d (%lf,%lf) , in circle %d"), SampleID, Samplei, Samplej, CurPixelPtr->SampleInImage[SampleID]);
                        //FVector p(-1, (float(j) + 0.5 - Radius) / Radius, (-(float(i) + 0.5) + Radius) / Radius);
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
                        //UE_LOG(LogTemp, Warning, TEXT("PixelInfo : (%d,%d)"), i, j);
                        //UE_LOG(LogTemp, Warning, TEXT("p : (%lf,%lf,%lf)"), p.X, p.Y,p.Z);
                        //UE_LOG(LogTemp, Warning, TEXT("po : (%lf,%lf,%lf)"), po.X, po.Y, po.Z);
                        //UE_LOG(LogTemp, Warning, TEXT("pO : (%lf,%lf,%lf)"), pO.X, pO.Y, pO.Z);
                        //UE_LOG(LogTemp, Warning, TEXT("thetad, theta, alpha : (%lf,%lf,%lf)"), thetad, theta, alpha);
                        //UE_LOG(LogTemp, Warning, TEXT("OPNormal : (%lf,%lf,%lf)"), OPNormal.X, OPNormal.Y, OPNormal.Z);

                        for (int m = 0; m < PlaneArray.Num(); m++)
                        {
                            //归一化的空间坐标下的交点
                            bool WillIntersect = false;
                            FVector IntersectPointNormal = RayPlaneIntersection(FVector::ZeroVector, OPNormal, PlaneArray[m], WillIntersect);
                            //FVector IntersectPointNormal = FMath::RayPlaneIntersection(FVector::ZeroVector, OPNormal, PlaneArray[m]);

                            if (i == 122 && j == 197 && k == 3 && l == 2)
                            {
                                UE_LOG(LogTemp, Warning, TEXT("Panel % d , IntersectPointNormal : (%lf,%lf,%lf) , WillIntersect %d , incube %d"), 
                                    m, IntersectPointNormal.X, IntersectPointNormal.Y, IntersectPointNormal.Z, WillIntersect, IsPointInCube(IntersectPointNormal));
                            }

                            //当找到OP和2D图像的交点
                            if (WillIntersect && IsPointInCube(IntersectPointNormal))
                            {

                                //sample origin和5个panel构成的包围盒只能有1个交点 , 但是这个交点却可能在panel的拼接缝上 , 因此最多可能有3个panel space坐标
                                //UE_LOG(LogTemp, Warning, TEXT("IntersectPoint : (%lf,%lf,%lf)"), IntersectPoint.X, IntersectPoint.Y, IntersectPoint.Z);
                                CurPixelPtr->SampleInfosArray[SampleID].FSampleOriginPanelLocalSpaceNormal = IntersectPointNormal;

                                //局部空间坐标
                                FVector IntersectPoint = IntersectPointNormal * Radius;
                                //UE_LOG(LogTemp, Warning, TEXT("IntersectPoint : (%lf,%lf,%lf)"), IntersectPoint.X, IntersectPoint.Y, IntersectPoint.Z);
                                //连续的屏幕坐标 , 坐标原点在左上角 , 竖直朝下是i(x), 水平朝右是j(y)
                                FVector2D IncidentRayOrigin = LoclSpace2Panel(m, IntersectPoint);

                                if (int(IncidentRayOrigin.X) >= ImageWidth)
                                {
                                    IncidentRayOrigin.X = float(ImageWidth - 1) + 0.5;
                                    UE_LOG(LogTemp, Warning, TEXT("Pixel (%d,%d)"), CurPixelPtr->i, CurPixelPtr->j);
                                    UE_LOG(LogTemp, Warning, TEXT("IntersectPoint : (%lf,%lf,%lf)"), IntersectPoint.X, IntersectPoint.Y, IntersectPoint.Z);
                                }
                                if (int(IncidentRayOrigin.Y) >= ImageWidth)
                                {
                                    IncidentRayOrigin.Y = float(ImageWidth - 1) + 0.5;
                                }
                                CurPixelPtr->SampleInfosArray[SampleID].ISampleOriginPanelImageSpace.Add(
                                    ITexel(m, int(IncidentRayOrigin.X), int(IncidentRayOrigin.Y)));
                                if (i == 122 && j == 197 && k == 3 && l == 2)
                                {
                                    UE_LOG(LogTemp, Warning, TEXT("IntersectPoint : (%lf,%lf,%lf)"), IntersectPoint.X, IntersectPoint.Y, IntersectPoint.Z);
                                    UE_LOG(LogTemp, Warning, TEXT("IntersectRayorigin : (%d,%d)"), int(IncidentRayOrigin.X), int(IncidentRayOrigin.Y));
                                    UE_LOG(LogTemp, Warning, TEXT("Panel % d , IntersectPointNormal : (%lf,%lf,%lf)"), m, IntersectPointNormal.X, IntersectPointNormal.Y, IntersectPointNormal.Z);

                                    //UE_LOG(LogTemp, Warning, TEXT("texel panel id : %d , (%d,%d)"), 
                                    //    CurPixelPtr->ISampleOriginTexelImageSpace[SampleID].ID, 
                                    //    CurPixelPtr->ISampleOriginTexelImageSpace[SampleID].TexelPos.i, 
                                    //    CurPixelPtr->ISampleOriginTexelImageSpace[SampleID].TexelPos.j);
                                }

                                testaddpixelpoint++;
                            }
                        }
                    }
                }
            }
        }
    }
    UE_LOG(LogTemp, Warning, TEXT("testaddpixelpoint %d "), testaddpixelpoint);
    UE_LOG(LogTemp, Warning, TEXT("testIsPixelInCircle %d in the circle"), testIsPixelInCircle);
    UE_LOG(LogTemp, Warning, TEXT("TMap ImagingPixels length :  %d "), ImagingPixels.Num());
}

bool AFisheyeCamera::IsPixelInCircle(int PixelI, int PixelJ)
{
    float PixelCenterI = float(PixelI) + 0.5;
    float PixelCenterJ = float(PixelJ) + 0.5;
    FVector2D PixelCenter(PixelCenterI, PixelCenterJ);

    FVector2D ImageCenter(Radius, Radius);
    float Dist = (ImageCenter - PixelCenter).Size();
    //UE_LOG(LogTemp, Warning, TEXT("PixelCenter : (%lf,%lf),  Dist : %lf "), PixelCenter.X,PixelCenter.Y,Dist);    //1740388
    if (Dist <= Radius)
    {
        return true;
    }
    else
    {
        return false;
    }
}

FVector2D AFisheyeCamera::LoclSpace2Panel(int PanelID, FVector IntersectPoint)
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


bool AFisheyeCamera::IsSampleInCircle(float i, float j)
{
    FVector2D SamplePoint(i, j);

    FVector2D ImageCenter(Radius, Radius);
    float Dist = (ImageCenter - SamplePoint).Size();
    if (Dist <= Radius)
    {
        return true;
    }
    else
    {
        return false;
    }
}


FVector2D AFisheyeCamera::PixelCoord2ImageCoord(int PixelI, int PixelJ)
{
    //2D的image坐标系以成像面中心为原点 , 从左到右为y轴, 从下到上为z轴
    return FVector2D((float(PixelJ) + 0.5 - Radius) / Radius, -(float(PixelI) + 0.5) + Radius) / Radius;
}


//FMath::RayPlaneIntersection几个特点 :
//系数Distance可正可负 . 这就说明实际上是判断直线line和平面交点而不是射线ray和平面交点
//如果线面平行, 返回的FVector是(-nan(ind),-nan(ind),inf) , 所以要自己判断是否平行
//这个函数仅在Distance不为0和无穷大的时候才返回true
FVector AFisheyeCamera::RayPlaneIntersection(const FVector& RayOrigin, const FVector& RayDirection, const FPlane& Plane, bool& WillIntersection)
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

bool AFisheyeCamera::IsPointInCube(FVector Point)
{
    if ((FMath::Abs(Point.X) >= 0.0) && (Point.X <= 1.0)
        && (Point.Y >= -1.0) && (Point.Y <= 1.0)
        && (Point.Z >= -1.0) && (Point.Z <= 1.0))
    {
        return true;
    }
    return false;
}


FActorDefinition AFisheyeCamera::GetSensorDefinition()
{
    constexpr bool bEnableModifyingPostProcessEffects = true;
    return UActorBlueprintFunctionLibrary::MakeCameraDefinition(
        TEXT("fisheye"),
        bEnableModifyingPostProcessEffects);
}

void AFisheyeCamera::Set(const FActorDescription &Description)
{
    Super::Set(Description);
    //djw tbd
    UActorBlueprintFunctionLibrary::SetCamera(Description, this);
}

void AFisheyeCamera::SetImageSize(int Width)
{
    ImageWidth = Width;
    Radius = float(ImageWidth) / 2;
}

void AFisheyeCamera::SetSSAA(int Num)
{
    SampleNum = Num;
    SampleDist = 1.0 / (2.0 * float(SampleNum));
}

void AFisheyeCamera::SetProjectionModel(int Model)
{
    ProjectionModel = Model;
}
















void AFisheyeCamera::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Super::EndPlay(EndPlayReason);
    FISHEYE_COUNTER = 0u;
}

// =============================================================================
// -- Local static functions implementations -----------------------------------
// =============================================================================

namespace FisheyeCamera_local_ns {

    static void SetCameraDefaultOverrides(USceneCaptureComponent2D &CaptureComponent2D)
    {
        auto &PostProcessSettings = CaptureComponent2D.PostProcessSettings;

        // Exposure
        PostProcessSettings.bOverride_AutoExposureMethod = true;
        PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
        PostProcessSettings.bOverride_AutoExposureBias = true;
        PostProcessSettings.bOverride_AutoExposureMinBrightness = true;
        PostProcessSettings.bOverride_AutoExposureMaxBrightness = true;
        PostProcessSettings.bOverride_AutoExposureSpeedUp = true;
        PostProcessSettings.bOverride_AutoExposureSpeedDown = true;
        PostProcessSettings.bOverride_AutoExposureCalibrationConstant = true;

        // Camera
        PostProcessSettings.bOverride_CameraShutterSpeed = true;
        PostProcessSettings.bOverride_CameraISO = true;
        PostProcessSettings.bOverride_DepthOfFieldFstop = true;
        PostProcessSettings.bOverride_DepthOfFieldMinFstop = true;
        PostProcessSettings.bOverride_DepthOfFieldBladeCount = true;

        // Film (Tonemapper)
        PostProcessSettings.bOverride_FilmSlope = true;
        PostProcessSettings.bOverride_FilmToe = true;
        PostProcessSettings.bOverride_FilmShoulder = true;
        PostProcessSettings.bOverride_FilmWhiteClip = true;
        PostProcessSettings.bOverride_FilmBlackClip = true;

        // Motion blur
        PostProcessSettings.bOverride_MotionBlurAmount = true;
        PostProcessSettings.MotionBlurAmount = 0.45f;
        PostProcessSettings.bOverride_MotionBlurMax = true;
        PostProcessSettings.MotionBlurMax = 0.35f;
        PostProcessSettings.bOverride_MotionBlurPerObjectSize = true;
        PostProcessSettings.MotionBlurPerObjectSize = 0.1f;

        // Color Grading
        PostProcessSettings.bOverride_WhiteTemp = true;
        PostProcessSettings.bOverride_WhiteTint = true;

        // Chromatic Aberration
        PostProcessSettings.bOverride_SceneFringeIntensity = true;
        PostProcessSettings.bOverride_ChromaticAberrationStartOffset = true;

        // Ambient Occlusion
        PostProcessSettings.bOverride_AmbientOcclusionIntensity = true;
        PostProcessSettings.AmbientOcclusionIntensity = 0.5f;
        PostProcessSettings.bOverride_AmbientOcclusionRadius = true;
        PostProcessSettings.AmbientOcclusionRadius = 100.0f;
        PostProcessSettings.bOverride_AmbientOcclusionStaticFraction = true;
        PostProcessSettings.AmbientOcclusionStaticFraction = 1.0f;
        PostProcessSettings.bOverride_AmbientOcclusionFadeDistance = true;
        PostProcessSettings.AmbientOcclusionFadeDistance = 50000.0f;
        PostProcessSettings.bOverride_AmbientOcclusionPower = true;
        PostProcessSettings.AmbientOcclusionPower = 2.0f;
        PostProcessSettings.bOverride_AmbientOcclusionBias = true;
        PostProcessSettings.AmbientOcclusionBias = 3.0f;
        PostProcessSettings.bOverride_AmbientOcclusionQuality = true;
        PostProcessSettings.AmbientOcclusionQuality = 100.0f;

        // Bloom
        PostProcessSettings.bOverride_BloomMethod = true;
        PostProcessSettings.BloomMethod = EBloomMethod::BM_SOG;
        PostProcessSettings.bOverride_BloomIntensity = true;
        PostProcessSettings.BloomIntensity = 0.3f;
        PostProcessSettings.bOverride_BloomThreshold = true;
        PostProcessSettings.BloomThreshold = -1.0f;
    }

    // Remove the show flags that might interfere with post-processing effects
    // like depth and semantic segmentation.
    static void ConfigureShowFlags(FEngineShowFlags &ShowFlags, bool bPostProcessing)
    {
        if (bPostProcessing)
        {
            ShowFlags.EnableAdvancedFeatures();
            ShowFlags.SetMotionBlur(true);
            return;
        }

        ShowFlags.SetAmbientOcclusion(false);
        ShowFlags.SetAntiAliasing(false);
        ShowFlags.SetVolumetricFog(false); // ShowFlags.SetAtmosphericFog(false);
        // ShowFlags.SetAudioRadius(false);
        // ShowFlags.SetBillboardSprites(false);
        ShowFlags.SetBloom(false);
        // ShowFlags.SetBounds(false);
        // ShowFlags.SetBrushes(false);
        // ShowFlags.SetBSP(false);
        // ShowFlags.SetBSPSplit(false);
        // ShowFlags.SetBSPTriangles(false);
        // ShowFlags.SetBuilderBrush(false);
        // ShowFlags.SetCameraAspectRatioBars(false);
        // ShowFlags.SetCameraFrustums(false);
        ShowFlags.SetCameraImperfections(false);
        ShowFlags.SetCameraInterpolation(false);
        // ShowFlags.SetCameraSafeFrames(false);
        // ShowFlags.SetCollision(false);
        // ShowFlags.SetCollisionPawn(false);
        // ShowFlags.SetCollisionVisibility(false);
        ShowFlags.SetColorGrading(false);
        // ShowFlags.SetCompositeEditorPrimitives(false);
        // ShowFlags.SetConstraints(false);
        // ShowFlags.SetCover(false);
        // ShowFlags.SetDebugAI(false);
        // ShowFlags.SetDecals(false);
        // ShowFlags.SetDeferredLighting(false);
        ShowFlags.SetDepthOfField(false);
        ShowFlags.SetDiffuse(false);
        ShowFlags.SetDirectionalLights(false);
        ShowFlags.SetDirectLighting(false);
        // ShowFlags.SetDistanceCulledPrimitives(false);
        // ShowFlags.SetDistanceFieldAO(false);
        // ShowFlags.SetDistanceFieldGI(false);
        ShowFlags.SetDynamicShadows(false);
        // ShowFlags.SetEditor(false);
        ShowFlags.SetEyeAdaptation(false);
        ShowFlags.SetFog(false);
        // ShowFlags.SetGame(false);
        // ShowFlags.SetGameplayDebug(false);
        // ShowFlags.SetGBufferHints(false);
        ShowFlags.SetGlobalIllumination(false);
        ShowFlags.SetGrain(false);
        // ShowFlags.SetGrid(false);
        // ShowFlags.SetHighResScreenshotMask(false);
        // ShowFlags.SetHitProxies(false);
        ShowFlags.SetHLODColoration(false);
        ShowFlags.SetHMDDistortion(false);
        // ShowFlags.SetIndirectLightingCache(false);
        // ShowFlags.SetInstancedFoliage(false);
        // ShowFlags.SetInstancedGrass(false);
        // ShowFlags.SetInstancedStaticMeshes(false);
        // ShowFlags.SetLandscape(false);
        // ShowFlags.SetLargeVertices(false);
        ShowFlags.SetLensFlares(false);
        ShowFlags.SetLevelColoration(false);
        ShowFlags.SetLightComplexity(false);
        ShowFlags.SetLightFunctions(false);
        ShowFlags.SetLightInfluences(false);
        ShowFlags.SetLighting(false);
        ShowFlags.SetLightMapDensity(false);
        ShowFlags.SetLightRadius(false);
        ShowFlags.SetLightShafts(false);
        // ShowFlags.SetLOD(false);
        ShowFlags.SetLODColoration(false);
        // ShowFlags.SetMaterials(false);
        // ShowFlags.SetMaterialTextureScaleAccuracy(false);
        // ShowFlags.SetMeshEdges(false);
        // ShowFlags.SetMeshUVDensityAccuracy(false);
        // ShowFlags.SetModeWidgets(false);
        ShowFlags.SetMotionBlur(false);
        // ShowFlags.SetNavigation(false);
        ShowFlags.SetOnScreenDebug(false);
        // ShowFlags.SetOutputMaterialTextureScales(false);
        // ShowFlags.SetOverrideDiffuseAndSpecular(false);
        // ShowFlags.SetPaper2DSprites(false);
        ShowFlags.SetParticles(false);
        // ShowFlags.SetPivot(false);
        ShowFlags.SetPointLights(false);
        // ShowFlags.SetPostProcessing(false);
        // ShowFlags.SetPostProcessMaterial(false);
        // ShowFlags.SetPrecomputedVisibility(false);
        // ShowFlags.SetPrecomputedVisibilityCells(false);
        // ShowFlags.SetPreviewShadowsIndicator(false);
        // ShowFlags.SetPrimitiveDistanceAccuracy(false);
        ShowFlags.SetPropertyColoration(false);
        // ShowFlags.SetQuadOverdraw(false);
        // ShowFlags.SetReflectionEnvironment(false);
        // ShowFlags.SetReflectionOverride(false);
        ShowFlags.SetRefraction(false);
        // ShowFlags.SetRendering(false);
        ShowFlags.SetSceneColorFringe(false);
        // ShowFlags.SetScreenPercentage(false);
        ShowFlags.SetScreenSpaceAO(false);
        ShowFlags.SetScreenSpaceReflections(false);
        // ShowFlags.SetSelection(false);
        // ShowFlags.SetSelectionOutline(false);
        // ShowFlags.SetSeparateTranslucency(false);
        // ShowFlags.SetShaderComplexity(false);
        // ShowFlags.SetShaderComplexityWithQuadOverdraw(false);
        // ShowFlags.SetShadowFrustums(false);
        // ShowFlags.SetSkeletalMeshes(false);
        // ShowFlags.SetSkinCache(false);
        ShowFlags.SetSkyLighting(false);
        // ShowFlags.SetSnap(false);
        // ShowFlags.SetSpecular(false);
        // ShowFlags.SetSplines(false);
        ShowFlags.SetSpotLights(false);
        // ShowFlags.SetStaticMeshes(false);
        ShowFlags.SetStationaryLightOverlap(false);
        // ShowFlags.SetStereoRendering(false);
        // ShowFlags.SetStreamingBounds(false);
        ShowFlags.SetSubsurfaceScattering(false);
        // ShowFlags.SetTemporalAA(false);
        // ShowFlags.SetTessellation(false);
        // ShowFlags.SetTestImage(false);
        // ShowFlags.SetTextRender(false);
        // ShowFlags.SetTexturedLightProfiles(false);
        ShowFlags.SetTonemapper(false);
        // ShowFlags.SetTranslucency(false);
        // ShowFlags.SetVectorFields(false);
        // ShowFlags.SetVertexColors(false);
        // ShowFlags.SetVignette(false);
        // ShowFlags.SetVisLog(false);
        // ShowFlags.SetVisualizeAdaptiveDOF(false);
        // ShowFlags.SetVisualizeBloom(false);
        ShowFlags.SetVisualizeBuffer(false);
        ShowFlags.SetVisualizeDistanceFieldAO(false);
        ShowFlags.SetVisualizeDistanceFieldGI(false);
        ShowFlags.SetVisualizeDOF(false);
        ShowFlags.SetVisualizeHDR(false);
        ShowFlags.SetVisualizeLightCulling(false);
        ShowFlags.SetVisualizeLPV(false);
        ShowFlags.SetVisualizeMeshDistanceFields(false);
        ShowFlags.SetVisualizeMotionBlur(false);
        ShowFlags.SetVisualizeOutOfBoundsPixels(false);
        ShowFlags.SetVisualizeSenses(false);
        ShowFlags.SetVisualizeShadingModels(false);
        ShowFlags.SetVisualizeSSR(false);
        ShowFlags.SetVisualizeSSS(false);
        // ShowFlags.SetVolumeLightingSamples(false);
        // ShowFlags.SetVolumes(false);
        // ShowFlags.SetWidgetComponents(false);
        // ShowFlags.SetWireframe(false);
    }

} // namespace SceneCaptureSensor_local_ns


void AFisheyeCamera::SendFisheyePixelsInRenderThread(AFisheyeCamera &Sensor)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureRenderTarget[i] != nullptr);
    }
    // Enqueue a command in the render-thread that will write the image buffer to
    // the data stream. The stream is created in the capture thus executed in the
    // game-thread.

    ENQUEUE_RENDER_COMMAND(FWriteFisheyePixelsToBuffer_SendPixelsInRenderThread)
        (
            [&Sensor, Stream = this->GetDataStream(Sensor)](auto &InRHICmdList) mutable
    {
        if (!Sensor.IsPendingKill())
        {

            auto t1 = std::chrono::system_clock::now();

            auto Buffer = Stream.PopBufferFromPool();
            //carla::Buffer *Buffers[5];
            //for (int i = 0; i < 5; i++)
            //{
            //    Buffers[i] = Stream.PopBufferFromPool();
            //}

            auto t2 = std::chrono::system_clock::now();

            Sensor.WriteFisheyePixelsToBuffer(
                Buffer,
                carla::sensor::SensorRegistry::get<AFisheyeCamera *>::type::header_offset,
                Sensor,
                InRHICmdList);

            auto t3 = std::chrono::system_clock::now();

            Stream.Send(Sensor, std::move(Buffer));

            auto t4 = std::chrono::system_clock::now();
            std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;
            std::chrono::duration<double> elapsed_seconds_2 = t3 - t2;
            std::chrono::duration<double> elapsed_seconds_3 = t4 - t3;
            //WritePixelsToBuffer��ʱ5ms
            UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time SendPixelsInRenderThread  , %.8lf, %.8lf, %.8lf"), elapsed_seconds_1.count(), elapsed_seconds_2.count(), elapsed_seconds_3.count());
        }
    }
    );
}

void AFisheyeCamera::WriteFisheyePixelsToBuffer(
    carla::Buffer &Buffer,
    uint32 Offset,
    AFisheyeCamera &Sensor,
    FRHICommandListImmediate &
#if CARLA_WITH_VULKAN_SUPPORT == 1
    InRHICmdList
#endif // CARLA_WITH_VULKAN_SUPPORT
)
{
    check(IsInRenderingThread());
    auto t1 = std::chrono::system_clock::now();

    FRHITexture2D *Texture[5];
    const uint32 BytesPerPixel = 4u; // PF_R8G8B8A8
    uint32 Width[5];
    uint32 Height[5];
    uint32 ExpectedStride[5];
    uint32 SrcStride[5];
    uint8 *Source[5];
    for(int i=0;i<5;i++)
    {
        Texture[i] = CaptureRenderTarget[i]->GetRenderTargetResource()->GetRenderTargetTexture();
        checkf(Texture[i] != nullptr, TEXT("FPixelReader: FisheyeCamera UTextureRenderTarget2D No.%d missing render target texture"), i);
        Width[i] = Texture[i]->GetSizeX();
        Height[i] = Texture[i]->GetSizeY();
        ExpectedStride[i] = Width[i] * BytesPerPixel;
        Source[i] = (reinterpret_cast<uint8*>(RHILockTexture2D(Texture[i], 0, RLM_ReadOnly, SrcStride[i], false)));
    }
    //for (int i = 0; i < 5; i++)
    //{
    //    Texture[i] = CaptureRenderTarget[i]->GetRenderTargetResource()->GetRenderTargetTexture();
    //    checkf(Texture[i] != nullptr, TEXT("FPixelReader: FisheyeCamera UTextureRenderTarget2D No.%d missing render target texture"), i);
    //    Width[i] = Texture[i]->GetSizeX();
    //    Height[i] = Texture[i]->GetSizeY();
    //    ExpectedStride[i] = Width[i] * BytesPerPixel;
    //    Source[i] = (reinterpret_cast<uint8*>(RHILockTexture2D(Texture[i], 0, RLM_ReadOnly, SrcStride[i], false)));

    //    //ExpectedStride[i] : 1080 * 4 = 4320 , SrcStride[i] : 4352
    //    UE_LOG(LogTemp, Warning, TEXT("Jarvan AFisheyeCamera No.%d ,Width:%d,Height:%d,ExpectedStride:%d,SrcStride:%d"),i, Width[i], Height[i], ExpectedStride[i], SrcStride[i]);
    //}
    auto t2 = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;

    int PixelColorExpectedStride = ImageWidth + (SrcStride[0] - ExpectedStride[0]) / BytesPerPixel;
#ifdef PLATFORM_WINDOWS
    // JB: Direct 3D uses additional rows in the buffer, so we need check the
    // result stride from the lock:
    //d3d平台单独处理是因为每一行末尾会有额外的数据 , 而其他api没有 , 所以其他api可以整块直接复制
    //经过测试, 每行末尾比原数据多了32字节
if (IsD3DPlatform(GMaxRHIShaderPlatform, false) && (ExpectedStride[0] != SrcStride[0]))
{
    PixelColorExpectedStride = ImageWidth + (SrcStride[0] - ExpectedStride[0]) / BytesPerPixel;
}
#endif // PLATFORM_WINDOWS

    Buffer.reset(Offset + ExpectedStride[0] * Height[0]);
    auto DstRow = Buffer.begin() + Offset;
    ParallelFor(ImaginPixelsQuerry.Num(), [&](int i)
    {
        auto& ImagingPixel = ImagingPixels[ImaginPixelsQuerry[i]];
        uint32 R = 0;
        uint32 G = 0;
        uint32 B = 0;
        uint32 A = 0;
        uint8 *OriginPtr;
        //遍历所有采样点 , 最后pixel最终颜色为所有采样点颜色的平均值
        for (int k = 0; k < SampleNum; k++)
        {
            for (int l = 0; l < SampleNum; l++)
            {
                int SampleID = k * SampleNum + l;
                if (ImagingPixel.SampleInImage[SampleID] != 0)
                {
                    uint32 RS = 0;
                    uint32 GS = 0;
                    uint32 BS = 0;
                    uint32 AS = 0;
                    auto& SampleOrigin = ImagingPixel.SampleInfosArray[SampleID].ISampleOriginPanelImageSpace;
                    //采样点在边界时 , 取边界两个点或三个点的平均颜色
                    for (int m = 0; m < SampleOrigin.Num(); m++)
                    {
                        int x = SampleOrigin[m].TexelPos.i;
                        int y = SampleOrigin[m].TexelPos.j;
                        OriginPtr = Source[SampleOrigin[m].ID];
                        RS += *(OriginPtr + (x * PixelColorExpectedStride + y) * 4u);
                        GS += *(OriginPtr + (x * PixelColorExpectedStride + y) * 4u + 1);
                        BS += *(OriginPtr + (x * PixelColorExpectedStride + y) * 4u + 2);
                        AS += *(OriginPtr + (x * PixelColorExpectedStride + y) * 4u + 3);
                    }
                    R += (RS / SampleOrigin.Num());
                    G += (GS / SampleOrigin.Num());
                    B += (BS / SampleOrigin.Num());
                    A += (AS / SampleOrigin.Num());
                }
            }
        }
        uint8 Color[4];
        Color[0] = R / (SampleNum * SampleNum);
        Color[1] = G / (SampleNum * SampleNum);
        Color[2] = B / (SampleNum * SampleNum);
        Color[3] = A / (SampleNum * SampleNum);
        *(DstRow + (ImagingPixel.i * ImageWidth + ImagingPixel.j) * 4u) = R / (SampleNum * SampleNum);
        *(DstRow + (ImagingPixel.i * ImageWidth + ImagingPixel.j) * 4u + 1) = G / (SampleNum * SampleNum);
        *(DstRow + (ImagingPixel.i * ImageWidth + ImagingPixel.j) * 4u + 2) = B / (SampleNum * SampleNum);
        *(DstRow + (ImagingPixel.i * ImageWidth + ImagingPixel.j) * 4u + 3) = A / (SampleNum * SampleNum);
    });
    auto t3 = std::chrono::system_clock::now();

    std::chrono::duration<double> elapsed_seconds_2 = t3 - t2;
    //  LockTexture Lock(Texture, SrcStride);耗时3ms
    UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time AFisheyeCamera::WriteFisheyePixelsToBuffer , %.8lf, %.8lf, %.8lf"), elapsed_seconds_1.count(), elapsed_seconds_2.count());


    //TArray<uint8> Color(DstRow + Offset, ImageWidth * ImageWidth * 4);
    //FDateTime Time = FDateTime::Now();
    //int64 Timestamp = Time.ToUnixTimestamp();
    //FString TimestampStr = FString::FromInt(Timestamp);
    //FString SaveFileName = FPaths::ProjectSavedDir();
    //SaveFileName.Append(FString("FishEye"));
    //SaveFileName.Append(TimestampStr);
    //SaveFileName.Append(".jpg");
    //ColorToImage(SaveFileName, Color, ImageWidth * 4, ImageWidth);

    for(int i=0;i<5;i++)
    {
        RHIUnlockTexture2D(Texture[i], 0, false);
    }
}


void AFisheyeCamera::SetExposureMethod(EAutoExposureMethod Method)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureMethod = Method;
    }
}

EAutoExposureMethod AFisheyeCamera::GetExposureMethod() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureMethod;
}

void AFisheyeCamera::SetExposureCompensation(float Compensation)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureBias = Compensation;
    }
}

float AFisheyeCamera::GetExposureCompensation() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureBias;
}

void AFisheyeCamera::SetShutterSpeed(float Speed)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.CameraShutterSpeed = Speed;
    }
}

float AFisheyeCamera::GetShutterSpeed() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.CameraShutterSpeed;
}

void AFisheyeCamera::SetISO(float ISO)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.CameraISO = ISO;
    }
}

float AFisheyeCamera::GetISO() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.CameraISO;
}

void AFisheyeCamera::SetAperture(float Aperture)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldFstop = Aperture;
    }
}

float AFisheyeCamera::GetAperture() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldFstop;
}

void AFisheyeCamera::SetFocalDistance(float Distance)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldFocalDistance = Distance;
    }
}

float AFisheyeCamera::GetFocalDistance() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldFocalDistance;
}

void AFisheyeCamera::SetDepthBlurAmount(float Amount)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldDepthBlurAmount = Amount;
    }
}

float AFisheyeCamera::GetDepthBlurAmount() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldDepthBlurAmount;
}

void AFisheyeCamera::SetDepthBlurRadius(float Radius)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldDepthBlurRadius = Radius;
    }
}

float AFisheyeCamera::GetDepthBlurRadius() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldDepthBlurRadius;
}

void AFisheyeCamera::SetDepthOfFieldMinFstop(float MinFstop)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldMinFstop = MinFstop;
    }
}

float AFisheyeCamera::GetDepthOfFieldMinFstop() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldMinFstop;
}

void AFisheyeCamera::SetBladeCount(int Count)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldBladeCount = Count;
    }
}

int AFisheyeCamera::GetBladeCount() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldBladeCount;
}

void AFisheyeCamera::SetFilmSlope(float Slope)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmSlope = Slope;
    }
}

float AFisheyeCamera::GetFilmSlope() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmSlope;
}

void AFisheyeCamera::SetFilmToe(float Toe)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmToe = Toe; // FilmToeAmount?
    }
}

float AFisheyeCamera::GetFilmToe() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmToe;
}

void AFisheyeCamera::SetFilmShoulder(float Shoulder)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmShoulder = Shoulder;
    }
}

float AFisheyeCamera::GetFilmShoulder() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmShoulder;
}

void AFisheyeCamera::SetFilmBlackClip(float BlackClip)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmBlackClip = BlackClip;
    }
}

float AFisheyeCamera::GetFilmBlackClip() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmBlackClip;
}

void AFisheyeCamera::SetFilmWhiteClip(float WhiteClip)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmWhiteClip = WhiteClip;
    }
}

float AFisheyeCamera::GetFilmWhiteClip() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmWhiteClip;
}

void AFisheyeCamera::SetExposureMinBrightness(float Brightness)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureMinBrightness = Brightness;
    }
}

float AFisheyeCamera::GetExposureMinBrightness() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureMinBrightness;
}

void AFisheyeCamera::SetExposureMaxBrightness(float Brightness)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureMaxBrightness = Brightness;
    }
}

float AFisheyeCamera::GetExposureMaxBrightness() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureMaxBrightness;
}

void AFisheyeCamera::SetExposureSpeedDown(float Speed)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureSpeedDown = Speed;
    }
}

float AFisheyeCamera::GetExposureSpeedDown() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureSpeedDown;
}

void AFisheyeCamera::SetExposureSpeedUp(float Speed)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureSpeedUp = Speed;
    }
}

float AFisheyeCamera::GetExposureSpeedUp() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureSpeedUp;
}

void AFisheyeCamera::SetExposureCalibrationConstant(float Constant)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureCalibrationConstant = Constant;
    }
}

float AFisheyeCamera::GetExposureCalibrationConstant() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureCalibrationConstant;
}

void AFisheyeCamera::SetMotionBlurIntensity(float Intensity)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.MotionBlurAmount = Intensity;
    }
}

float AFisheyeCamera::GetMotionBlurIntensity() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.MotionBlurAmount;
}

void AFisheyeCamera::SetMotionBlurMaxDistortion(float MaxDistortion)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.MotionBlurMax = MaxDistortion;
    }
}

float AFisheyeCamera::GetMotionBlurMaxDistortion() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.MotionBlurMax;
}

void AFisheyeCamera::SetMotionBlurMinObjectScreenSize(float ScreenSize)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.MotionBlurPerObjectSize = ScreenSize;
    }
}

float AFisheyeCamera::GetMotionBlurMinObjectScreenSize() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.MotionBlurPerObjectSize;
}

void AFisheyeCamera::SetWhiteTemp(float Temp)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.WhiteTemp = Temp;
    }
}

float AFisheyeCamera::GetWhiteTemp() const
{
    check(CaptureComponent2D != nullptr);
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.WhiteTemp;
}

void AFisheyeCamera::SetWhiteTint(float Tint)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.WhiteTint = Tint;
    }
}

float AFisheyeCamera::GetWhiteTint() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.WhiteTint;
}

void AFisheyeCamera::SetChromAberrIntensity(float Intensity)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.SceneFringeIntensity = Intensity;
    }
}

float AFisheyeCamera::GetChromAberrIntensity() const
{
    check(CaptureComponent2D != nullptr);
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.SceneFringeIntensity;
}

void AFisheyeCamera::SetChromAberrOffset(float Offset)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.ChromaticAberrationStartOffset = Offset;
    }
}

float AFisheyeCamera::GetChromAberrOffset() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.ChromaticAberrationStartOffset;
}