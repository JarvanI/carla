#include "Carla.h"
#include "FisheyeCameraCS4.h"
#include "FisheyeCS4Camera/Public/FisheyeCS4CameraRendering.h"
#include "Carla/Game/CarlaStatics.h"
#include "Engine/Classes/Engine/Scene.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "FileHelper.h"
#include "ImageUtils.h"
#include "ModuleManager.h"
#include "ParallelFor.h"
#include "Actor/ActorBlueprintFunctionLibrary.h"

static auto FISHEYECS4_COUNTER = 0u;

// =============================================================================
// -- Local static methods -----------------------------------------------------
// =============================================================================

// Local namespace to avoid name collisions on unit builds.
namespace FisheyeCameraCS4_local_ns {

    static void SetCameraDefaultOverrides(USceneCaptureComponent2D &CaptureComponent2D);

    static void ConfigureShowFlags(FEngineShowFlags &ShowFlags, bool bPostProcessing = true);

    static auto GetQualitySettings(UWorld *World)
    {
        auto Settings = UCarlaStatics::GetCarlaSettings(World);
        check(Settings != nullptr);
        return Settings->GetQualityLevel();
    }
} // namespace FisheyeCameraCS4_local_ns

// =============================================================================
// -- AFisheyeCameraCS4 ------------------------------------------------------
// =============================================================================

AFisheyeCameraCS4::AFisheyeCameraCS4(const FObjectInitializer &ObjectInitializer)
    : Super(ObjectInitializer)
{
    // Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PrePhysics; // After CameraManager's TG_PrePhysics.

    for (int i = 0; i < SnitchNum; ++i)
    {
        CaptureComponent2D.Add(CreateDefaultSubobject<USceneCaptureComponent2D>(
            FName(*FString::Printf(TEXT("AFisheyeCameraCS4SceneCaptureComponent2D_%d"), i))));
        CaptureComponent2D[i]->FOVAngle = 90;
        CaptureComponent2D[i]->SetupAttachment(RootComponent);
    }
    
    ++FISHEYECS4_COUNTER;
}

void AFisheyeCameraCS4::BeginPlay()
{
    const bool bInForceLinearGamma = !bEnablePostProcessingEffects;
    if(SnitchNum == 4){
        //Left
        CaptureComponent2D[0]->SetRelativeRotation(FRotator(0, -45, 0));
        //Right
        CaptureComponent2D[1]->SetRelativeRotation(FRotator(0, 45, 0));
        //Top
        CaptureComponent2D[2]->SetRelativeRotation(FRotator(90, 0, 45));
        //Bottom
        CaptureComponent2D[3]->SetRelativeRotation(FRotator(-90, 0, 45));
        CaptureComponent2D[4]->bCaptureEveryFrame = false;
        CaptureComponent2D[4]->Deactivate();
    }else if(SnitchNum == 5){
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
    }

    SampleTextureWidth = FMath::Min(ImageWidth, ImageHeight);
    for (int i = 0; i < SnitchNum; ++i)
    {
        CaptureRenderTargets.Add(NewObject<UTextureRenderTarget2D>(this));
        CaptureRenderTargets[i]->CompressionSettings = TextureCompressionSettings::TC_VectorDisplacementmap;
        CaptureRenderTargets[i]->SRGB = false;
        CaptureRenderTargets[i]->bAutoGenerateMips = true;
        CaptureRenderTargets[i]->bForceLinearGamma = true;
        //CaptureRenderTargets[i]->MipGenSettings = TMGS_SimpleAverage;
        
        CaptureRenderTargets[i]->AddressX = TextureAddress::TA_Clamp;
        CaptureRenderTargets[i]->AddressY = TextureAddress::TA_Clamp;
        CaptureRenderTargets[i]->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
        CaptureRenderTargets[i]->InitCustomFormat(SampleTextureWidth, SampleTextureWidth, PF_FloatRGBA, true);
        if (bEnablePostProcessingEffects)
        {
            CaptureRenderTargets[i]->TargetGamma = TargetGamma;
        }

        check(IsValid(CaptureComponent2D[i]) && !CaptureComponent2D[i]->IsPendingKill());
        CaptureComponent2D[i]->Deactivate();
        CaptureComponent2D[i]->TextureTarget = CaptureRenderTargets[i];
        CaptureComponent2D[i]->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
        CaptureComponent2D[i]->ShowFlags.Vignette = 0;
        CaptureComponent2D[i]->ShowFlags.Bloom = 0;
        //CaptureComponent2D[i]->ShowFlags.MotionBlur = 0;
        //CaptureComponent2D[i]->ShowFlags.Tonemapper = 0;
        CaptureComponent2D[i]->ShowFlags.EyeAdaptation = 0;
        CaptureComponent2D[i]->ShowFlags.TemporalAA = 0;
        CaptureComponent2D[i]->ShowFlags.SkipTonemapper = 0;
        CaptureComponent2D[i]->UpdateContent();
        CaptureComponent2D[i]->Activate();

        FisheyeCameraCS4_local_ns::ConfigureShowFlags(CaptureComponent2D[i]->ShowFlags, bEnablePostProcessingEffects);
    }

    FishEyeTexture = NewObject<UTextureRenderTarget2D>(this);
    FishEyeTexture->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
    FishEyeTexture->bAutoGenerateMips = false;
    FishEyeTexture->InitCustomFormat(ImageWidth, ImageHeight, PF_FloatRGBA, !bEnablePostProcessingEffects);
    FishEyeTexture->UpdateResourceImmediate(true);

    CaptureRenderTarget = NewObject<UTextureRenderTarget2D>(this);
    CaptureRenderTarget->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
    CaptureRenderTarget->bAutoGenerateMips = false;
    CaptureRenderTarget->InitCustomFormat(ImageWidth, ImageHeight, PF_B8G8R8A8, !bEnablePostProcessingEffects);
    CaptureRenderTarget->UpdateResourceImmediate(true);

    int MipWidth = ImageWidth;
    int MipHeight = ImageHeight;
    for (int CurrentMipmapLevel = 0; CurrentMipmapLevel < MipLevel; ++CurrentMipmapLevel)
    {
        MipWidth =  FMath::DivideAndRoundUp(MipWidth, 2);
        MipHeight = FMath::DivideAndRoundUp(MipHeight, 2);
        MipBloomRenderTarget.Add(NewObject<UTextureRenderTarget2D>(this));
        MipBloomRenderTarget[CurrentMipmapLevel]->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
        MipBloomRenderTarget[CurrentMipmapLevel]->bAutoGenerateMips = false;
        MipBloomRenderTarget[CurrentMipmapLevel]->InitCustomFormat(MipWidth, MipHeight, PF_FloatRGBA, !bEnablePostProcessingEffects);
        MipBloomRenderTarget[CurrentMipmapLevel]->UpdateResourceImmediate(true);
    }

    Radius = float(SampleTextureWidth) / 2;
    SampleDist = 1.0 / (2.0 * float(SampleNum));

    FisheyeCS4CameraRenderingPtr = NewObject<UFisheyeCS4CameraRendering>(this);
    auto t1 = std::chrono::system_clock::now();
    FisheyeCS4CameraRenderingPtr->CalPixelsRelationship(FIntPoint(ImageWidth, ImageHeight), SnitchNum,
        FOV,d1,d2,d3,d4,fx,fy,cx,cy);
    auto t2 = std::chrono::system_clock::now();
    std::chrono::duration<double> CalPixelsRelationshipTime = t2 - t1;
    UE_LOG(LogTemp, Warning, TEXT("SnitchNum %d Width : %d , cost time CalPixelsRelationshipTime : %.8lf"), 
        SnitchNum, ImageWidth, CalPixelsRelationshipTime.count());

    // Make sure that there is enough time in the render queue.
    UKismetSystemLibrary::ExecuteConsoleCommand(
        GetWorld(),
        FString("g.TimeoutForBlockOnRenderFence 300000"));

    // This ensures the camera is always spawning the rain drops in case the
    // weather was previously set to has rain
    GetEpisode().GetWeather()->NotifyWeather();
    Super::BeginPlay();
}

void AFisheyeCameraCS4::PrePhysTick(float DeltaTime)
{
    Super::PrePhysTick(DeltaTime);
    // Add the view information every tick. Its only used for one tick and then
    // removed by the streamer.
     //IStreamingManager::Get().AddViewInformation(
     //    CaptureComponent2D[0]->GetComponentLocation(),
     //    ImageWidth,
     //    ImageWidth / FMath::Tan(CaptureComponent2D[0]->FOVAngle));

}

void AFisheyeCameraCS4::PostPhysTick(UWorld* World, ELevelTick TickType, float DeltaTime)
{
    //FDateTime Time = FDateTime::Now();
    //int64 Timestamp = Time.ToUnixTimestamp();
    //FString TimestampStr = FString::FromInt(Timestamp);

    //auto& Setting = CaptureComponent2D[0]->PostProcessSettings;
    //TArray<UFisheyeCS4CameraRendering::FBloomStage> BloomStages =
    //{
    //    { Setting.Bloom6Size, Setting.Bloom6Tint },
    //    { Setting.Bloom5Size, Setting.Bloom5Tint },
    //    { Setting.Bloom4Size, Setting.Bloom4Tint },
    //    { Setting.Bloom3Size, Setting.Bloom3Tint },
    //    { Setting.Bloom2Size, Setting.Bloom2Tint },
    //    { Setting.Bloom1Size, Setting.Bloom1Tint }
    //};
    //FisheyeCS4CameraRenderingPtr->UseComputeShaderArray(CaptureRenderTargets, FishEyeTexture, CaptureRenderTarget, MipBloomRenderTarget, BloomStages, 4, cx, cy);

    FPixelReader::SendPixelsInRenderThread<AFisheyeCameraCS4, FColor>(*this);

}
void AFisheyeCameraCS4::WaitForRenderThreadToFinish() {
    //TRACE_CPUPROFILER_EVENT_SCOPE(AFisheyeCameraCS4::WaitForRenderThreadToFinish);
    // FlushRenderingCommands();
}
void AFisheyeCameraCS4::EnqueueRenderSceneImmediate()
{
    //FDateTime Time = FDateTime::Now();
    //int64 Timestamp = Time.ToUnixTimestamp();
    //FString TimestampStr = FString::FromInt(Timestamp);

    auto& Setting = CaptureComponent2D[0]->PostProcessSettings;
    TArray<UFisheyeCS4CameraRendering::FBloomStage> BloomStages =
    {
        { Setting.Bloom6Size, Setting.Bloom6Tint },
        { Setting.Bloom5Size, Setting.Bloom5Tint },
        { Setting.Bloom4Size, Setting.Bloom4Tint },
        { Setting.Bloom3Size, Setting.Bloom3Tint },
        { Setting.Bloom2Size, Setting.Bloom2Tint },
        { Setting.Bloom1Size, Setting.Bloom1Tint }
    };
    FisheyeCS4CameraRenderingPtr->UseComputeShaderArray(
        CaptureRenderTargets, 
        CaptureRenderTarget,
        FishEyeTexture, 
        MipBloomRenderTarget, 
        BloomStages, 
        4, 
        cx, cy);

}


FActorDefinition AFisheyeCameraCS4::GetSensorDefinition()
{
    constexpr bool bEnableModifyingPostProcessEffects = true;
    return UActorBlueprintFunctionLibrary::MakeFisheyeCameraCS4Definition(
        TEXT("fisheyecs4"),
        bEnableModifyingPostProcessEffects);
}

void AFisheyeCameraCS4::Set(const FActorDescription &Description)
{
    Super::Set(Description);
    UActorBlueprintFunctionLibrary::SetCamera(Description, this);
}

void AFisheyeCameraCS4::SetImageWidth(int W)
{
    ImageWidth = W;
}

void AFisheyeCameraCS4::SetImageHeight(int H)
{
    ImageHeight = H;
}

void AFisheyeCameraCS4::SetSnitchNum(int Num)
{
    SnitchNum = Num;
}

void AFisheyeCameraCS4::SetFOV(float fov)
{
    FOV = fov;
}

void AFisheyeCameraCS4::Setd1(float d_1)
{
    d1 = d_1;
}
void AFisheyeCameraCS4::Setd2(float d_2)
{
    d2 = d_2;
}
void AFisheyeCameraCS4::Setd3(float d_3)
{
    d3 = d_3;
}
void AFisheyeCameraCS4::Setd4(float d_4)
{
    d4 = d_4;
}
void AFisheyeCameraCS4::Setfx(float f_x)
{
    fx = f_x;
}
void AFisheyeCameraCS4::Setfy(float f_y)
{
    fy = f_y;
}
void AFisheyeCameraCS4::Setcx(float c_x)
{
    cx = c_x;
}
void AFisheyeCameraCS4::Setcy(float c_y)
{
    cy = c_y;
}

void AFisheyeCameraCS4::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Super::EndPlay(EndPlayReason);
    FISHEYECS4_COUNTER = 0u;
}

// =============================================================================
// -- Local static functions implementations -----------------------------------
// =============================================================================

namespace FisheyeCameraCS4_local_ns {

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
        //PostProcessSettings.bOverride_AutoExposureCalibrationConstant = true;

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
        //ShowFlags.SetVisualizeDistanceFieldGI(false);
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



void AFisheyeCameraCS4::SetExposureMethod(EAutoExposureMethod Method)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureMethod = Method;
    }
}

EAutoExposureMethod AFisheyeCameraCS4::GetExposureMethod() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureMethod;
}

void AFisheyeCameraCS4::SetExposureCompensation(float Compensation)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureBias = Compensation;
    }
}

float AFisheyeCameraCS4::GetExposureCompensation() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureBias;
}

void AFisheyeCameraCS4::SetShutterSpeed(float Speed)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.CameraShutterSpeed = Speed;
    }
}

float AFisheyeCameraCS4::GetShutterSpeed() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.CameraShutterSpeed;
}

void AFisheyeCameraCS4::SetISO(float ISO)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.CameraISO = ISO;
    }
}

float AFisheyeCameraCS4::GetISO() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.CameraISO;
}

void AFisheyeCameraCS4::SetAperture(float Aperture)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldFstop = Aperture;
    }
}

float AFisheyeCameraCS4::GetAperture() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldFstop;
}

void AFisheyeCameraCS4::SetFocalDistance(float Distance)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldFocalDistance = Distance;
    }
}

float AFisheyeCameraCS4::GetFocalDistance() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldFocalDistance;
}

void AFisheyeCameraCS4::SetDepthBlurAmount(float Amount)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldDepthBlurAmount = Amount;
    }
}

float AFisheyeCameraCS4::GetDepthBlurAmount() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldDepthBlurAmount;
}

void AFisheyeCameraCS4::SetDepthBlurRadius(float Radius)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldDepthBlurRadius = Radius;
    }
}

float AFisheyeCameraCS4::GetDepthBlurRadius() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldDepthBlurRadius;
}

void AFisheyeCameraCS4::SetDepthOfFieldMinFstop(float MinFstop)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldMinFstop = MinFstop;
    }
}

float AFisheyeCameraCS4::GetDepthOfFieldMinFstop() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldMinFstop;
}

void AFisheyeCameraCS4::SetBladeCount(int Count)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldBladeCount = Count;
    }
}

int AFisheyeCameraCS4::GetBladeCount() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldBladeCount;
}

void AFisheyeCameraCS4::SetFilmSlope(float Slope)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmSlope = Slope;
    }
}

float AFisheyeCameraCS4::GetFilmSlope() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmSlope;
}

void AFisheyeCameraCS4::SetFilmToe(float Toe)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmToe = Toe; // FilmToeAmount?
    }
}

float AFisheyeCameraCS4::GetFilmToe() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmToe;
}

void AFisheyeCameraCS4::SetFilmShoulder(float Shoulder)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmShoulder = Shoulder;
    }
}

float AFisheyeCameraCS4::GetFilmShoulder() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmShoulder;
}

void AFisheyeCameraCS4::SetFilmBlackClip(float BlackClip)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmBlackClip = BlackClip;
    }
}

float AFisheyeCameraCS4::GetFilmBlackClip() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmBlackClip;
}

void AFisheyeCameraCS4::SetFilmWhiteClip(float WhiteClip)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmWhiteClip = WhiteClip;
    }
}

float AFisheyeCameraCS4::GetFilmWhiteClip() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmWhiteClip;
}

void AFisheyeCameraCS4::SetExposureMinBrightness(float Brightness)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureMinBrightness = Brightness;
    }
}

float AFisheyeCameraCS4::GetExposureMinBrightness() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureMinBrightness;
}

void AFisheyeCameraCS4::SetExposureMaxBrightness(float Brightness)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureMaxBrightness = Brightness;
    }
}

float AFisheyeCameraCS4::GetExposureMaxBrightness() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureMaxBrightness;
}

void AFisheyeCameraCS4::SetExposureSpeedDown(float Speed)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureSpeedDown = Speed;
    }
}

float AFisheyeCameraCS4::GetExposureSpeedDown() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureSpeedDown;
}

void AFisheyeCameraCS4::SetExposureSpeedUp(float Speed)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureSpeedUp = Speed;
    }
}

float AFisheyeCameraCS4::GetExposureSpeedUp() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureSpeedUp;
}

void AFisheyeCameraCS4::SetExposureCalibrationConstant(float Constant)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        //CaptureComponent2D[i]->PostProcessSettings.AutoExposureCalibrationConstant = Constant;
    }
}

float AFisheyeCameraCS4::GetExposureCalibrationConstant() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    //return CaptureComponent2D[0]->PostProcessSettings.AutoExposureCalibrationConstant;
    return 0.0f;
}

void AFisheyeCameraCS4::SetMotionBlurIntensity(float Intensity)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.MotionBlurAmount = Intensity;
    }
}

float AFisheyeCameraCS4::GetMotionBlurIntensity() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.MotionBlurAmount;
}

void AFisheyeCameraCS4::SetMotionBlurMaxDistortion(float MaxDistortion)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.MotionBlurMax = MaxDistortion;
    }
}

float AFisheyeCameraCS4::GetMotionBlurMaxDistortion() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.MotionBlurMax;
}

void AFisheyeCameraCS4::SetMotionBlurMinObjectScreenSize(float ScreenSize)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.MotionBlurPerObjectSize = ScreenSize;
    }
}

float AFisheyeCameraCS4::GetMotionBlurMinObjectScreenSize() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.MotionBlurPerObjectSize;
}

void AFisheyeCameraCS4::SetWhiteTemp(float Temp)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.WhiteTemp = Temp;
    }
}

float AFisheyeCameraCS4::GetWhiteTemp() const
{
    check(CaptureComponent2D.Num() != 0);
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.WhiteTemp;
}

void AFisheyeCameraCS4::SetWhiteTint(float Tint)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.WhiteTint = Tint;
    }
}

float AFisheyeCameraCS4::GetWhiteTint() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.WhiteTint;
}

void AFisheyeCameraCS4::SetChromAberrIntensity(float Intensity)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.SceneFringeIntensity = Intensity;
    }
}

float AFisheyeCameraCS4::GetChromAberrIntensity() const
{
    check(CaptureComponent2D.Num() != 0);
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.SceneFringeIntensity;
}

void AFisheyeCameraCS4::SetChromAberrOffset(float Offset)
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.ChromaticAberrationStartOffset = Offset;
    }
}

float AFisheyeCameraCS4::GetChromAberrOffset() const
{
    for (int i = 0; i < SnitchNum; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.ChromaticAberrationStartOffset;
}