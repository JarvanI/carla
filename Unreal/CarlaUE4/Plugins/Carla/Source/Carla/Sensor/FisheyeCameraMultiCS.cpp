// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma de Barcelona (UAB). This work is licensed under the terms of the MIT license. For a copy, see <https://opensource.org/licenses/MIT>.



#include "Carla.h"
#include "Carla/Game/CarlaStatics.h"
#include "Components/DrawFrustumComponent.h"
#include "Engine/Classes/Engine/Scene.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneCaptureComponent2DMulti.h"
#include "Carla/Sensor/FisheyeCameraMultiCS.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HighResScreenshot.h"
#include "ContentStreaming.h"
#include "ImageUtils.h"
#include "ModuleManager.h"
#include "Actor/ActorBlueprintFunctionLibrary.h"
#include "FileHelper.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ParallelFor.h"
#include "FisheyeCameraCS.h"
#include "NewShaderPlugin/Public/NewShaderPluginRendering.h"

uint32 AFisheyeCameraMultiCS::IDGenerator = 100u;

// =============================================================================
// -- AFisheyeCameraMultiCS ------------------------------------------------------
// =============================================================================


AFisheyeCameraMultiCS::AFisheyeCameraMultiCS(const FObjectInitializer &ObjectInitializer)
    : Super(ObjectInitializer)
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PrePhysics;

    FishEyeTexture = NewObject<UTextureRenderTarget2D>();
    check(FishEyeTexture);
    //FishEyeTexture->RenderTargetFormat = RTF_RGBA32f;
    FishEyeTexture->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
    FishEyeTexture->bAutoGenerateMips = false;
    //FishEyeTexture->InitAutoFormat(1080, 1080);
    FishEyeTexture->InitCustomFormat(ImageWidth, ImageWidth, PF_B8G8R8A8, !bEnablePostProcessingEffects);
    FishEyeTexture->UpdateResourceImmediate(true);

    NewShaderRenderingPtr = NewObject<UNewShaderRendering>();
    Radius = float(ImageWidth) / 2;
    SampleDist = 1.0 / (2.0 * float(SampleNum));
}

void AFisheyeCameraMultiCS::BeginPlay() {
    // SetUpSceneCaptureComponent是个虚函数 , 在ASceneCaptureSensor中里面什么也没有
    // 只有ASceneCaptureSensor的子类 , 比如AShaderBasedSensor , 才会重写SetUpSceneCaptureComponent函数 , 
    // 然后在这里执行 . 是为了调用PostProcessSettings
    // Call derived classes to set up their things.
    //SetUpSceneCaptureComponent(*CaptureComponent2D);

    // Make sure that there is enough time in the render queue.
    UKismetSystemLibrary::ExecuteConsoleCommand(
        GetWorld(),
        FString("g.TimeoutForBlockOnRenderFence 300000"));

    // This ensures the camera is always spawning the rain drops in case the
    // weather was previously set to has rain
    GetEpisode().GetWeather()->NotifyWeather();
    Super::BeginPlay();
}

void AFisheyeCameraMultiCS::Tick(float DeltaTime) {
    Super::Tick(DeltaTime);
    // Add the view information every tick. Its only used for one tick and then
    // removed by the streamer.
    //IStreamingManager::Get().AddViewInformation(
    //	GetActorLocation(),
    //	GetImageWidth(),
    //	GetImageHeight() / FMath::Tan(GetFOVAngle()));
    //	GetPosInRendertarget().Min.X , GetPosInRendertarget().Min.Y ,
    //	GetPosInRendertarget().Max.X ,GetPosInRendertarget().Max.Y);
    //FPixelReader::SendSplitPixelsInRenderThread(*this, GetPosInRendertarget());

    //FDateTime Time = FDateTime::Now();
    //int64 Timestamp = Time.ToUnixTimestamp();
    //FString TimestampStr = FString::FromInt(Timestamp);

    //FString SaveFileNameMulti = FPaths::ProjectSavedDir();
    //SaveFileNameMulti.Append("AFishEyeMultiCSBefore");
    //SaveFileNameMulti.Append(TimestampStr);
    //SaveFileNameMulti.Append(".jpg");
    //ScreenshotToImage2D(SaveFileNameMulti, CaptureRenderTarget);

    TArray<UTextureRenderTarget2D*> CaptureRenderTargetArray;
    CaptureRenderTargetArray.Add(CaptureRenderTarget);
    NewShaderRenderingPtr->UseComputeShaderArray(CaptureRenderTargetArray, FishEyeTexture, 4);

    //FString SaveFileName = FPaths::ProjectSavedDir();
    //SaveFileName.Append(FString("AFishEyeMultiAfter"));
    //SaveFileName.Append(TimestampStr);
    //SaveFileName.Append(".jpg");
    //ScreenshotToImage2D(SaveFileName, FishEyeTexture);
    SendFisheyeCameraMultiCSPixelsInRenderThread(*this);
};

void AFisheyeCameraMultiCS::EndPlay(const EEndPlayReason::Type EndPlayReason) {
    Super::EndPlay(EndPlayReason);
    //CaptureComponent2DMulti->Deactivate();
    for (int i = 0; i < 5; i++)
    {
        CaptureComponent2DMulti->CameraAttributeMap.FindAndRemoveChecked(--IDGenerator);
    }
};

FActorDefinition AFisheyeCameraMultiCS::GetSensorDefinition()
{
    constexpr bool bEnableModifyingPostProcessEffects = true;
    return UActorBlueprintFunctionLibrary::MakeCameraDefinition(
        TEXT("fisheyemultics"),
        bEnableModifyingPostProcessEffects);
}

void AFisheyeCameraMultiCS::Set(const FActorDescription &Description)
{
    Super::Set(Description);
    UActorBlueprintFunctionLibrary::SetCamera(Description, this);
    for (int i = 0; i < 5; i++)
    {
        FString name_fstring = FString("FisheyeMultiCSSceneComponent_") + FString::FromInt(i);
        FName name = FName(*name_fstring);
        CameraSceneComponent[i] = NewObject<USceneComponent>(this, name);
        CameraSceneComponent[i]->RegisterComponent();
        CameraSceneComponent[i]->AttachToComponent(this->RootComponent, FAttachmentTransformRules::KeepRelativeTransform);

        FRotator Rot;
        switch (i)
        {
        case 0:
            Rot = FRotator(0, 0, 0);
            break;
        case 3:
            Rot = FRotator(0, -90, 0);
            break;
        case 4:
            Rot = FRotator(0, 90, 0);
            break;
        case 1:
            Rot = FRotator(90, 0, 0);
            break;
        case 2:
            Rot = FRotator(-90, 0, 0);
            break;
        }
        CameraSceneComponent[i]->SetRelativeRotation(Rot);

        FCameraAttribute CameraAttr;
        CameraAttr.SceneComponent = CameraSceneComponent[i];
        CameraAttr.FOVAngle = 90.0f;
        CameraAttr.OrthoWidth = 512;
        CameraAttr.ProjectionType = ECameraProjectionMode::Perspective;
        CameraAttr.bUseCustomProjectionMatrix = false;
        CameraAttr.CustomProjectionMatrix.SetIdentity();
        CameraAttr.ClipPlaneNormal = FVector(0, 0, 1);
        CameraAttr.bOverride_CustomNearClippingPlane = false;
        CameraAttr.SizeX = ImageWidth;
        CameraAttr.SizeY = ImageWidth;
        CaptureComponent2DMulti->AddCamera(IDGenerator++, CameraAttr);
    }
}

void AFisheyeCameraMultiCS::SetImageSize(int Width)
{
    ImageWidth = Width;
    Radius = float(ImageWidth) / 2;
}

void AFisheyeCameraMultiCS::SetSSAA(int Num)
{
    SampleNum = Num;
    SampleDist = 1.0 / (2.0 * float(SampleNum));
}

void AFisheyeCameraMultiCS::SetProjectionModel(int Model)
{
    ProjectionModel = Model;
}

void AFisheyeCameraMultiCS::ScreenshotToImage2D(const FString& InImagePath, UTextureRenderTarget2D* TextureTarget)
{

    if (TextureTarget)
    {
        //auto start = FDateTime::Now().GetTimeOfDay().GetTotalMilliseconds();
        FTextureRenderTargetResource* TextureRenderTargetResource = TextureTarget->GameThread_GetRenderTargetResource();
        //auto mid = FDateTime::Now().GetTimeOfDay().GetTotalMilliseconds();
        int32 Width = TextureTarget->SizeX;
        int32 Height = TextureTarget->SizeY;
        if (Width > 0 && Height > 0) {
            TArray<FColor> OutData;
            TextureRenderTargetResource->ReadPixels(OutData, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), FIntRect(0, 0, Width, Height));
            //auto mid2 = FDateTime::Now().GetTimeOfDay().GetTotalMilliseconds();
            ColorToImage(InImagePath, OutData, Width, Height);
            //auto mid3 = FDateTime::Now().GetTimeOfDay().GetTotalMilliseconds();
            //UE_LOG(LogTemp, Warning, TEXT("mid - start : %f, mid2-mid %f, mid3-mid2 %f"), mid - start, mid2 - mid,mid3-mid2);
        }
    }
    else {
        UE_LOG(LogTemp, Warning, TEXT("AFisheyeCameraMultiCS::ScreenshotToImage2D ::NO CaptureComponent2D->TextureTarget"));
    }
}

void AFisheyeCameraMultiCS::ColorToImage(const FString& InImagePath, TArray<FColor> InColor, int32 InWidth, int32 InHeight)
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

void AFisheyeCameraMultiCS::SendFisheyeCameraMultiCSPixelsInRenderThread(AFisheyeCameraMultiCS &Sensor)
{
    check(Sensor.FishEyeTexture != nullptr);
    // Enqueue a command in the render-thread that will write the image buffer to
    // the data stream. The stream is created in the capture thus executed in the
    // game-thread.

    ENQUEUE_RENDER_COMMAND(FWriteFisheyeCameraCSPixelsToBuffer_SendPixelsInRenderThread)
        (
            [&Sensor, Stream = this->GetDataStream(Sensor)](auto &InRHICmdList) mutable
    {
        if (!Sensor.IsPendingKill())
        {

            auto t1 = std::chrono::system_clock::now();

            auto Buffer = Stream.PopBufferFromPool();

            auto t2 = std::chrono::system_clock::now();

            Sensor.WriteFisheyeCameraMultiCSPixelsToBuffer(
                Buffer,
                carla::sensor::SensorRegistry::get<AFisheyeCameraCS *>::type::header_offset,
                Sensor,
                InRHICmdList);

            auto t3 = std::chrono::system_clock::now();

            Stream.Send(Sensor, std::move(Buffer));

            auto t4 = std::chrono::system_clock::now();
            std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;
            std::chrono::duration<double> elapsed_seconds_2 = t3 - t2;
            std::chrono::duration<double> elapsed_seconds_3 = t4 - t3;
            //WritePixelsToBuffer��ʱ5ms
            UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time SendFisheyeCameraCSPixelsInRenderThread  , %.8lf, %.8lf, %.8lf"), elapsed_seconds_1.count(), elapsed_seconds_2.count(), elapsed_seconds_3.count());
        }
    }
    );
}

void AFisheyeCameraMultiCS::WriteFisheyeCameraMultiCSPixelsToBuffer(
    carla::Buffer &Buffer,
    uint32 Offset,
    AFisheyeCameraMultiCS &Sensor,
    FRHICommandListImmediate &InRHICmdList)
{

    check(IsInRenderingThread());

    auto t1 = std::chrono::system_clock::now();

    //#if CARLA_WITH_VULKAN_SUPPORT == 1
    //    if (IsVulkanPlatform(GMaxRHIShaderPlatform))
    //    {
    //        UE_LOG(LogTemp, Warning, TEXT("Jarvan FPixelReader::WritePixelsToBuffer( IsVulkanPlatform"));
    //        WritePixelsToBuffer_Vulkan(RenderTarget, Buffer, Offset, InRHICmdList);
    //        return;
    //    }
    //#endif // CARLA_WITH_VULKAN_SUPPORT

    FRHITexture2D *Texture = Sensor.FishEyeTexture->GetRenderTargetResource()->GetRenderTargetTexture();
    checkf(Texture != nullptr, TEXT("AFisheyeCameraCS::WriteFisheyeCameraCSPixelsToBuffer: UTextureRenderTarget2D missing render target texture"));

    const uint32 BytesPerPixel = 4u; // PF_R8G8B8A8
    const uint32 Width = Texture->GetSizeX();
    const uint32 Height = Texture->GetSizeY();
    const uint32 ExpectedStride = Width * BytesPerPixel;

    uint32 SrcStride;
    // 这个函数结束后Lock自动析构 , 就释放了RHILockTexture2D锁
    uint8 *Source = (reinterpret_cast<uint8*>(RHILockTexture2D(Texture, 0, RLM_ReadOnly, SrcStride, false)));


    auto t2 = std::chrono::system_clock::now();

#ifdef PLATFORM_WINDOWS
    // JB: Direct 3D uses additional rows in the buffer, so we need check the
    // result stride from the lock:
    //d3d平台单独处理是因为每一行末尾会有额外的数据 , 而其他api没有 , 所以这里可以整块直接复制
    if (IsD3DPlatform(GMaxRHIShaderPlatform, false) && (ExpectedStride != SrcStride))
    {
        UE_LOG(LogTemp, Warning, TEXT("Jarvan FPixelReader::WritePixelsToBuffer( IsD3DPlatform"));
        Buffer.reset(Offset + ExpectedStride * Height);
        auto DstRow = Buffer.begin() + Offset;
        const uint8 *SrcRow = Source;
        for (uint32 Row = 0u; Row < Height; ++Row)
        {
            //每一次只从SrcRow拷贝ExpectedStride长度到DstRow , 说明ExpectedStride < SrcStride ,
            //而且说明SrcStride比ExpectedStride多出来的那一截是在尾部 , 可以抛弃
            FMemory::Memcpy(DstRow, SrcRow, ExpectedStride);
            DstRow += ExpectedStride;
            SrcRow += SrcStride;
        }
    }
    else
#endif // PLATFORM_WINDOWS
    {
        UE_LOG(LogTemp, Warning, TEXT("Jarvan FPixelReader::WritePixelsToBuffer( Buffer.copy_from"));
        check(ExpectedStride == SrcStride);
        //const uint8 *Source = Lock.Source;
        //这里会创建boost::asio::buffer里的const_buffer , 不可修改buffer里的数据
        Buffer.copy_from(Offset, Source, ExpectedStride * Height);
    }
    //test 1
    //Buffer.reset(Offset + ExpectedStride * Height);
    //auto DstRow = Buffer.begin() + Offset;
    //const uint8 *SrcRow = Source;
    //for (uint32 Row = 0u; Row < Height; ++Row)
    //{
    //    //每一次只从SrcRow拷贝ExpectedStride长度到DstRow , 说明ExpectedStride < SrcStride ,
    //    //而且说明SrcStride比ExpectedStride多出来的那一截是在尾部 , 可以抛弃
    //    FMemory::Memcpy(DstRow, SrcRow, ExpectedStride);
    //    DstRow += ExpectedStride;
    //    SrcRow += SrcStride;
    //}

    //test 2
    //Buffer.copy_from(Offset, Source, ExpectedStride * Height);

    auto t3 = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;
    std::chrono::duration<double> elapsed_seconds_2 = t3 - t2;
    //  LockTexture Lock(Texture, SrcStride);耗时3ms
    RHIUnlockTexture2D(Texture, 0, false);
    UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time WritePixelsToBuffer , %.8lf, %.8lf, %.8lf"), elapsed_seconds_1.count(), elapsed_seconds_2.count());
}
























void AFisheyeCameraMultiCS::SetExposureMethod(EAutoExposureMethod Method)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.AutoExposureMethod = Method;
}

EAutoExposureMethod AFisheyeCameraMultiCS::GetExposureMethod() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.AutoExposureMethod;
}

void AFisheyeCameraMultiCS::SetExposureCompensation(float Compensation)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.AutoExposureBias = Compensation;
}

float AFisheyeCameraMultiCS::GetExposureCompensation() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.AutoExposureBias;
}

void AFisheyeCameraMultiCS::SetShutterSpeed(float Speed)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.CameraShutterSpeed = Speed;
}

float AFisheyeCameraMultiCS::GetShutterSpeed() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.CameraShutterSpeed;
}

void AFisheyeCameraMultiCS::SetISO(float ISO)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.CameraISO = ISO;
}

float AFisheyeCameraMultiCS::GetISO() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.CameraISO;
}

void AFisheyeCameraMultiCS::SetAperture(float Aperture)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldFstop = Aperture;
}

float AFisheyeCameraMultiCS::GetAperture() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldFstop;
}

void AFisheyeCameraMultiCS::SetFocalDistance(float Distance)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldFocalDistance = Distance;
}

float AFisheyeCameraMultiCS::GetFocalDistance() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldFocalDistance;
}

void AFisheyeCameraMultiCS::SetDepthBlurAmount(float Amount)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldDepthBlurAmount = Amount;
}

float AFisheyeCameraMultiCS::GetDepthBlurAmount() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldDepthBlurAmount;
}

void AFisheyeCameraMultiCS::SetDepthBlurRadius(float Radius)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldDepthBlurRadius = Radius;
}

float AFisheyeCameraMultiCS::GetDepthBlurRadius() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldDepthBlurRadius;
}

void AFisheyeCameraMultiCS::SetDepthOfFieldMinFstop(float MinFstop)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldMinFstop = MinFstop;
}

float AFisheyeCameraMultiCS::GetDepthOfFieldMinFstop() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldMinFstop;
}

void AFisheyeCameraMultiCS::SetBladeCount(int Count)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldBladeCount = Count;
}

int AFisheyeCameraMultiCS::GetBladeCount() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldBladeCount;
}

void AFisheyeCameraMultiCS::SetFilmSlope(float Slope)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.FilmSlope = Slope;
}

float AFisheyeCameraMultiCS::GetFilmSlope() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.FilmSlope;
}

void AFisheyeCameraMultiCS::SetFilmToe(float Toe)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.FilmToe = Toe; // FilmToeAmount?
}

float AFisheyeCameraMultiCS::GetFilmToe() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.FilmToe;
}

void AFisheyeCameraMultiCS::SetFilmShoulder(float Shoulder)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.FilmShoulder = Shoulder;
}

float AFisheyeCameraMultiCS::GetFilmShoulder() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.FilmShoulder;
}

void AFisheyeCameraMultiCS::SetFilmBlackClip(float BlackClip)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.FilmBlackClip = BlackClip;
}

float AFisheyeCameraMultiCS::GetFilmBlackClip() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.FilmBlackClip;
}

void AFisheyeCameraMultiCS::SetFilmWhiteClip(float WhiteClip)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.FilmWhiteClip = WhiteClip;
}

float AFisheyeCameraMultiCS::GetFilmWhiteClip() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.FilmWhiteClip;
}

void AFisheyeCameraMultiCS::SetExposureMinBrightness(float Brightness)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.AutoExposureMinBrightness = Brightness;
}

float AFisheyeCameraMultiCS::GetExposureMinBrightness() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.AutoExposureMinBrightness;
}

void AFisheyeCameraMultiCS::SetExposureMaxBrightness(float Brightness)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.AutoExposureMaxBrightness = Brightness;
}

float AFisheyeCameraMultiCS::GetExposureMaxBrightness() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.AutoExposureMaxBrightness;
}

void AFisheyeCameraMultiCS::SetExposureSpeedDown(float Speed)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.AutoExposureSpeedDown = Speed;
}

float AFisheyeCameraMultiCS::GetExposureSpeedDown() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.AutoExposureSpeedDown;
}

void AFisheyeCameraMultiCS::SetExposureSpeedUp(float Speed)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.AutoExposureSpeedUp = Speed;
}

float AFisheyeCameraMultiCS::GetExposureSpeedUp() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.AutoExposureSpeedUp;
}

void AFisheyeCameraMultiCS::SetExposureCalibrationConstant(float Constant)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.AutoExposureCalibrationConstant = Constant;
}

float AFisheyeCameraMultiCS::GetExposureCalibrationConstant() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.AutoExposureCalibrationConstant;
}

void AFisheyeCameraMultiCS::SetMotionBlurIntensity(float Intensity)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.MotionBlurAmount = Intensity;
}

float AFisheyeCameraMultiCS::GetMotionBlurIntensity() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.MotionBlurAmount;
}

void AFisheyeCameraMultiCS::SetMotionBlurMaxDistortion(float MaxDistortion)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.MotionBlurMax = MaxDistortion;
}

float AFisheyeCameraMultiCS::GetMotionBlurMaxDistortion() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.MotionBlurMax;
}

void AFisheyeCameraMultiCS::SetMotionBlurMinObjectScreenSize(float ScreenSize)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.MotionBlurPerObjectSize = ScreenSize;
}

float AFisheyeCameraMultiCS::GetMotionBlurMinObjectScreenSize() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.MotionBlurPerObjectSize;
}

void AFisheyeCameraMultiCS::SetWhiteTemp(float Temp)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.WhiteTemp = Temp;
}

float AFisheyeCameraMultiCS::GetWhiteTemp() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.WhiteTemp;
}

void AFisheyeCameraMultiCS::SetWhiteTint(float Tint)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.WhiteTint = Tint;
}

float AFisheyeCameraMultiCS::GetWhiteTint() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.WhiteTint;
}

void AFisheyeCameraMultiCS::SetChromAberrIntensity(float Intensity)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.SceneFringeIntensity = Intensity;
}

float AFisheyeCameraMultiCS::GetChromAberrIntensity() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.SceneFringeIntensity;
}

void AFisheyeCameraMultiCS::SetChromAberrOffset(float Offset)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.ChromaticAberrationStartOffset = Offset;
}

float AFisheyeCameraMultiCS::GetChromAberrOffset() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.ChromaticAberrationStartOffset;
}