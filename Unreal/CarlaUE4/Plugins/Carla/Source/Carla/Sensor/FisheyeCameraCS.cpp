// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma de Barcelona (UAB). This work is licensed under the terms of the MIT license. For a copy, see <https://opensource.org/licenses/MIT>.



#include "Carla.h"
#include "FisheyeCameraCS.h"
#include "ShadertestPlugin/Public/ShadertestPluginRendering.h"
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


static auto FISHEYECS_COUNTER = 0u;

// =============================================================================
// -- Local static methods -----------------------------------------------------
// =============================================================================

// Local namespace to avoid name collisions on unit builds.
namespace FisheyeCameraCS_local_ns {

    static void SetCameraDefaultOverrides(USceneCaptureComponent2D &CaptureComponent2D);

    static void ConfigureShowFlags(FEngineShowFlags &ShowFlags, bool bPostProcessing = true);

    static auto GetQualitySettings(UWorld *World)
    {
        auto Settings = UCarlaStatics::GetCarlaSettings(World);
        check(Settings != nullptr);
        return Settings->GetQualityLevel();
    }
} // namespace FisheyeCameraCS_local_ns

// =============================================================================
// -- AFisheyeCameraCS ------------------------------------------------------
// =============================================================================

AFisheyeCameraCS::AFisheyeCameraCS(const FObjectInitializer &ObjectInitializer)
: Super(ObjectInitializer)
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PrePhysics;

    for (int i = 0; i < 5; i++)
    {
        CaptureRenderTarget.Add(CreateDefaultSubobject<UTextureRenderTarget2D>(
            FName(*FString::Printf(TEXT("FisheyeCameraCSCaptureRenderTarget_%d"), i))));
        CaptureRenderTarget[i]->CompressionSettings = TextureCompressionSettings::TC_VectorDisplacementmap;
        CaptureRenderTarget[i]->SRGB = false;
        CaptureRenderTarget[i]->bAutoGenerateMips = false;
        CaptureRenderTarget[i]->AddressX = TextureAddress::TA_Clamp;
        CaptureRenderTarget[i]->AddressY = TextureAddress::TA_Clamp;
        CaptureRenderTarget[i]->SizeX = ImageWidth;
        CaptureRenderTarget[i]->SizeY = ImageWidth;

        CaptureComponent2D.Add(CreateDefaultSubobject<USceneCaptureComponent2D>(
            FName(*FString::Printf(TEXT("FisheyeCameraCSSceneCaptureComponent2D_%d"), i))));
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

    FishEyeTexture = NewObject<UTextureRenderTarget2D>();
    check(FishEyeTexture);
    //FishEyeTexture->RenderTargetFormat = RTF_RGBA32f;
    FishEyeTexture->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
    FishEyeTexture->bAutoGenerateMips = false;
    //FishEyeTexture->InitAutoFormat(1080, 1080);
    FishEyeTexture->InitCustomFormat(ImageWidth, ImageWidth, PF_B8G8R8A8, !bEnablePostProcessingEffects);
    FishEyeTexture->UpdateResourceImmediate(true);

    ShadertestRenderingPtr = NewObject<UShadertestRendering>();

    Radius = float(ImageWidth) / 2;
    SampleDist = 1.0 / (2.0 * float(SampleNum));

    //导致FisheyeTexture变暗的罪魁祸首
    //注释后原本很暗的合成鱼眼图像变正常 , 但是所有的原本的2D图像又过曝了
    //for (int i = 0; i < 5; i++)
    //{
    //    FisheyeCameraCS_local_ns::SetCameraDefaultOverrides(*CaptureComponent2D[i]);
    //}
    ++FISHEYECS_COUNTER;
}

void AFisheyeCameraCS::BeginPlay()
{
    const bool bInForceLinearGamma = !bEnablePostProcessingEffects;
    for (int i = 0; i < 5; i++) {
        if (bEnablePostProcessingEffects)
        {
            CaptureRenderTarget[i]->TargetGamma = TargetGamma;
        }
        CaptureRenderTarget[i]->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
        CaptureRenderTarget[i]->bAutoGenerateMips = false;
        CaptureRenderTarget[i]->InitCustomFormat(ImageWidth, ImageWidth, PF_B8G8R8A8, bInForceLinearGamma);
        check(IsValid(CaptureComponent2D[i]) && !CaptureComponent2D[i]->IsPendingKill());
        CaptureComponent2D[i]->Deactivate();
        CaptureComponent2D[i]->TextureTarget = CaptureRenderTarget[i];
        CaptureComponent2D[i]->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
        CaptureComponent2D[i]->UpdateContent();
        CaptureComponent2D[i]->Activate();

        FisheyeCameraCS_local_ns::ConfigureShowFlags(CaptureComponent2D[i]->ShowFlags, bEnablePostProcessingEffects);
    }

    // Make sure that there is enough time in the render queue.
    UKismetSystemLibrary::ExecuteConsoleCommand(
        GetWorld(),
        FString("g.TimeoutForBlockOnRenderFence 300000"));

    // This ensures the camera is always spawning the rain drops in case the
    // weather was previously set to has rain
    GetEpisode().GetWeather()->NotifyWeather();
    Super::BeginPlay();
}

void AFisheyeCameraCS::Tick(float DeltaTime)
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

    //    SaveFileName.Append(FString("FishEyeCSSplitNO"));
    //    SaveFileName.Append(FString::FromInt(i));
    //    SaveFileName.Append(TimestampStr);
    //    SaveFileName.Append(".jpg");
    //    ScreenshotToImage2D(SaveFileName, CaptureRenderTarget[i]);
    //}
    //FString SaveFileName = FPaths::ProjectSavedDir();
    //SaveFileName.Append(FString("FishEyeCS"));
    //SaveFileName.Append(TimestampStr);
    //SaveFileName.Append(".jpg");
    ShadertestRenderingPtr->UseComputeShaderArray(CaptureRenderTarget, FishEyeTexture, 4, ProjectionModel);

    //ScreenshotToImage2D(SaveFileName, FishEyeTexture);
    SendFisheyeCameraCSPixelsInRenderThread(*this);
}

void AFisheyeCameraCS::ScreenshotToImage2D(const FString& InImagePath, UTextureRenderTarget2D* TextureTarget)
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
        UE_LOG(LogTemp, Warning, TEXT("NO CaptureComponent2D->TextureTarget"));
    }
}

void AFisheyeCameraCS::ColorToImage(const FString& InImagePath, TArray<FColor> InColor, int32 InWidth, int32 InHeight)
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

void AFisheyeCameraCS::SendFisheyeCameraCSPixelsInRenderThread(AFisheyeCameraCS &Sensor)
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

            Sensor.WriteFisheyeCameraCSPixelsToBuffer(
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
            //UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time SendFisheyeCameraCSPixelsInRenderThread  , %.8lf, %.8lf, %.8lf"), elapsed_seconds_1.count(), elapsed_seconds_2.count(), elapsed_seconds_3.count());
        }
    }
    );
}

void AFisheyeCameraCS::WriteFisheyeCameraCSPixelsToBuffer(
    carla::Buffer &Buffer,
    uint32 Offset,
    AFisheyeCameraCS &Sensor,
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
        //UE_LOG(LogTemp, Warning, TEXT("Jarvan FPixelReader::WritePixelsToBuffer( IsD3DPlatform"));
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
        //UE_LOG(LogTemp, Warning, TEXT("Jarvan FPixelReader::WritePixelsToBuffer( Buffer.copy_from"));
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
    //UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time WritePixelsToBuffer , %.8lf, %.8lf, %.8lf"), elapsed_seconds_1.count(), elapsed_seconds_2.count());
}


FActorDefinition AFisheyeCameraCS::GetSensorDefinition()
{
    constexpr bool bEnableModifyingPostProcessEffects = true;
    return UActorBlueprintFunctionLibrary::MakeCameraDefinition(
        TEXT("fisheyecs"),
        bEnableModifyingPostProcessEffects);
}

void AFisheyeCameraCS::Set(const FActorDescription &Description)
{
    Super::Set(Description);
    //djw tbd
    UActorBlueprintFunctionLibrary::SetCamera(Description, this);
}

void AFisheyeCameraCS::SetImageSize(int Width)
{
    ImageWidth = Width;
    Radius = float(ImageWidth) / 2;
}

void AFisheyeCameraCS::SetSSAA(int Num)
{
    SampleNum = Num;
    SampleDist = 1.0 / (2.0 * float(SampleNum));
}

void AFisheyeCameraCS::SetProjectionModel(int Model)
{
    ProjectionModel = Model;
}

void AFisheyeCameraCS::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Super::EndPlay(EndPlayReason);
    FISHEYECS_COUNTER = 0u;
}

// =============================================================================
// -- Local static functions implementations -----------------------------------
// =============================================================================

namespace FisheyeCameraCS_local_ns {

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



void AFisheyeCameraCS::SetExposureMethod(EAutoExposureMethod Method)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureMethod = Method;
    }
}

EAutoExposureMethod AFisheyeCameraCS::GetExposureMethod() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureMethod;
}

void AFisheyeCameraCS::SetExposureCompensation(float Compensation)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureBias = Compensation;
    }
}

float AFisheyeCameraCS::GetExposureCompensation() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureBias;
}

void AFisheyeCameraCS::SetShutterSpeed(float Speed)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.CameraShutterSpeed = Speed;
    }
}

float AFisheyeCameraCS::GetShutterSpeed() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.CameraShutterSpeed;
}

void AFisheyeCameraCS::SetISO(float ISO)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.CameraISO = ISO;
    }
}

float AFisheyeCameraCS::GetISO() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.CameraISO;
}

void AFisheyeCameraCS::SetAperture(float Aperture)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldFstop = Aperture;
    }
}

float AFisheyeCameraCS::GetAperture() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldFstop;
}

void AFisheyeCameraCS::SetFocalDistance(float Distance)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldFocalDistance = Distance;
    }
}

float AFisheyeCameraCS::GetFocalDistance() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldFocalDistance;
}

void AFisheyeCameraCS::SetDepthBlurAmount(float Amount)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldDepthBlurAmount = Amount;
    }
}

float AFisheyeCameraCS::GetDepthBlurAmount() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldDepthBlurAmount;
}

void AFisheyeCameraCS::SetDepthBlurRadius(float Radius)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldDepthBlurRadius = Radius;
    }
}

float AFisheyeCameraCS::GetDepthBlurRadius() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldDepthBlurRadius;
}

void AFisheyeCameraCS::SetDepthOfFieldMinFstop(float MinFstop)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldMinFstop = MinFstop;
    }
}

float AFisheyeCameraCS::GetDepthOfFieldMinFstop() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldMinFstop;
}

void AFisheyeCameraCS::SetBladeCount(int Count)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldBladeCount = Count;
    }
}

int AFisheyeCameraCS::GetBladeCount() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldBladeCount;
}

void AFisheyeCameraCS::SetFilmSlope(float Slope)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmSlope = Slope;
    }
}

float AFisheyeCameraCS::GetFilmSlope() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmSlope;
}

void AFisheyeCameraCS::SetFilmToe(float Toe)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmToe = Toe; // FilmToeAmount?
    }
}

float AFisheyeCameraCS::GetFilmToe() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmToe;
}

void AFisheyeCameraCS::SetFilmShoulder(float Shoulder)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmShoulder = Shoulder;
    }
}

float AFisheyeCameraCS::GetFilmShoulder() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmShoulder;
}

void AFisheyeCameraCS::SetFilmBlackClip(float BlackClip)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmBlackClip = BlackClip;
    }
}

float AFisheyeCameraCS::GetFilmBlackClip() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmBlackClip;
}

void AFisheyeCameraCS::SetFilmWhiteClip(float WhiteClip)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmWhiteClip = WhiteClip;
    }
}

float AFisheyeCameraCS::GetFilmWhiteClip() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmWhiteClip;
}

void AFisheyeCameraCS::SetExposureMinBrightness(float Brightness)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureMinBrightness = Brightness;
    }
}

float AFisheyeCameraCS::GetExposureMinBrightness() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureMinBrightness;
}

void AFisheyeCameraCS::SetExposureMaxBrightness(float Brightness)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureMaxBrightness = Brightness;
    }
}

float AFisheyeCameraCS::GetExposureMaxBrightness() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureMaxBrightness;
}

void AFisheyeCameraCS::SetExposureSpeedDown(float Speed)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureSpeedDown = Speed;
    }
}

float AFisheyeCameraCS::GetExposureSpeedDown() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureSpeedDown;
}

void AFisheyeCameraCS::SetExposureSpeedUp(float Speed)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureSpeedUp = Speed;
    }
}

float AFisheyeCameraCS::GetExposureSpeedUp() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureSpeedUp;
}

void AFisheyeCameraCS::SetExposureCalibrationConstant(float Constant)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureCalibrationConstant = Constant;
    }
}

float AFisheyeCameraCS::GetExposureCalibrationConstant() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureCalibrationConstant;
}

void AFisheyeCameraCS::SetMotionBlurIntensity(float Intensity)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.MotionBlurAmount = Intensity;
    }
}

float AFisheyeCameraCS::GetMotionBlurIntensity() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.MotionBlurAmount;
}

void AFisheyeCameraCS::SetMotionBlurMaxDistortion(float MaxDistortion)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.MotionBlurMax = MaxDistortion;
    }
}

float AFisheyeCameraCS::GetMotionBlurMaxDistortion() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.MotionBlurMax;
}

void AFisheyeCameraCS::SetMotionBlurMinObjectScreenSize(float ScreenSize)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.MotionBlurPerObjectSize = ScreenSize;
    }
}

float AFisheyeCameraCS::GetMotionBlurMinObjectScreenSize() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.MotionBlurPerObjectSize;
}

void AFisheyeCameraCS::SetWhiteTemp(float Temp)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.WhiteTemp = Temp;
    }
}

float AFisheyeCameraCS::GetWhiteTemp() const
{
    check(CaptureComponent2D.Num() != 0);
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.WhiteTemp;
}

void AFisheyeCameraCS::SetWhiteTint(float Tint)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.WhiteTint = Tint;
    }
}

float AFisheyeCameraCS::GetWhiteTint() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.WhiteTint;
}

void AFisheyeCameraCS::SetChromAberrIntensity(float Intensity)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.SceneFringeIntensity = Intensity;
    }
}

float AFisheyeCameraCS::GetChromAberrIntensity() const
{
    check(CaptureComponent2D.Num() != 0);
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.SceneFringeIntensity;
}

void AFisheyeCameraCS::SetChromAberrOffset(float Offset)
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.ChromaticAberrationStartOffset = Offset;
    }
}

float AFisheyeCameraCS::GetChromAberrOffset() const
{
    for (int i = 0; i < 5; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.ChromaticAberrationStartOffset;
}