// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma de Barcelona (UAB). This work is licensed under the terms of the MIT license. For a copy, see <https://opensource.org/licenses/MIT>.


#include "MultiCameraManager.h"

#include "PixelReader.h"
#include "Components/SceneCaptureComponent2DMulti.h"
#include "Carla/Sensor/PixelsSplitRunnable.h"
#include "Engine/Engine.h"
#include "Engine/Player.h"

#include "HAL/RunnableThread.h" 
#include "Engine/TextureRenderTarget2D.h"

// Sets default values
AMultiCameraManager::AMultiCameraManager()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	CaptureRenderTarget = CreateDefaultSubobject<UTextureRenderTarget2D>(
		FName(*FString::Printf(TEXT("CaptureRenderTargetForMulti"))));
	CaptureRenderTarget->InitCustomFormat(100, 100, PF_B8G8R8A8, false);
	//CaptureRenderTarget->InitAutoFormat(100, 100);
	CaptureRenderTarget->TargetGamma = TargetGamma;
	//图片的压缩类型选择为默认
	CaptureRenderTarget->CompressionSettings = TextureCompressionSettings::TC_Default;
	CaptureRenderTarget->SRGB = false;
	CaptureRenderTarget->bAutoGenerateMips = false;
	//指定纹理处理方式 , 有wrap , clamp, mirror , max 这4种方式
	//wrap是指重复(平铺), mirror是特殊的wrap , 重复的图像是镜像的
	//这里的clamp指的是边缘拉伸 , 个人理解就是如果CaptureRenderTarget的长宽不符合viewport , 就拉伸
	CaptureRenderTarget->AddressX = TextureAddress::TA_Clamp;
	CaptureRenderTarget->AddressY = TextureAddress::TA_Clamp;

	CaptureComponent2DMulti = CreateDefaultSubobject<USceneCaptureComponent2DMulti>(
		FName(*FString::Printf(TEXT("USceneCaptureComponent2DMulti"))));
	CaptureComponent2DMulti->ShowFlags.DisableAdvancedFeatures();
	CaptureComponent2DMulti->TextureTarget = CaptureRenderTarget;
	CaptureComponent2DMulti->CompositeMode = ESceneCaptureCompositeMode::SCCM_Composite;
	CaptureComponent2DMulti->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	CaptureComponent2DMulti->SetupAttachment(RootComponent);
	SetCameraDefaultOverrides();
}

//USceneCaptureComponent2DMulti* AMultiCameraManager::GetCaptureComponent2DMulti() {
//	return CaptureComponent2DMulti;
//}


void AMultiCameraManager::CreateNewThread()
{
	//FString Name = FString(TEXT("PixelsSplitRunnable ")) + FString::FromInt(114514);
	PixelsSplitRunnable = FPixelsSplitRunnable::JoyInit(FString("dd123456"));
}


// Called when the game starts or when spawned
void AMultiCameraManager::BeginPlay()
{
	Super::BeginPlay();
	ConfigureShowFlags();
	//CreateNewThread();
	//PixelsSplitRunnable->PauseThread();
	
	//GetNetOwningPlayer()->ConsoleCommand("ShowFlag.Rendering 0");

	//FString MyCommandString = "ShowFlag.Rendering 0";
	//GEngine->Exec(GetWorld(), *MyCommandString);
}

 class test
 {
 public:
 	test(int a, int b, int c):A(a),B(b),C(c)
 	{
 		UE_LOG(LogTemp, Warning, TEXT("Jarvan test construct called, %d %d %d %p"), A, B, C, this);
 	}

 	test(const test &t)
 	{
 		A = t.A;
 		B = t.B;
 		C = t.C;
 		UE_LOG(LogTemp, Warning, TEXT("Jarvan test copy construct called, %d %d %d %p"), A, B, C, this);
 	}

 	test& operator=(const test &t)
 	{
 		A = t.A;
 		B = t.B;
 		C = t.C;
 		UE_LOG(LogTemp, Warning, TEXT("Jarvan test copy operator called, %d %d %d %p"), A, B, C, this);
 		return *this;
 	}

 	~test()
 	{
 		UE_LOG(LogTemp, Warning, TEXT("Jarvan test dis construct called, %d %d %d %p"), A, B, C, this);
 	}

 	int A;
 	int B;
 	int C;
 };


// Called every frame
void AMultiCameraManager::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	//this->GetWorld()->GetGameViewport()->bDisableWorldRendering = true;
	FPixelReader::SendAllSplitPixelsInRenderThread(MultiCameras);

	//auto t1 = std::chrono::system_clock::now();

	//UE_LOG(LogTemp, Warning, TEXT("jarvan count %d"), count);
	//FPixelsSplitRunnable* Runnable = new FPixelsSplitRunnable();
	//UE_LOG(LogTemp, Warning, TEXT("jarvan after new FPixelsSplitRunnable();"), count);

	//FRunnableThread* Thread = FRunnableThread::Create(Runnable, TEXT("MyThread %d")+count, 0 ,EThreadPriority::TPri_Highest);
	//UE_LOG(LogTemp, Warning, TEXT("jarvan after FRunnableThread::Create;"), count);

	//auto t2 = std::chrono::system_clock::now();

	//std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;
	//UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time AMultiCameraManager::Tick , %.8lf"), elapsed_seconds_1.count());;


	//if (IsInGameThread())
	//{
	//	UE_LOG(LogTemp, Warning, TEXT("jarvan InGameThread , AMultiCameraManager::Tick(float DeltaTime)"));
	//}
	//if (IsInRenderingThread())
	//{
	//	UE_LOG(LogTemp, Warning, TEXT("jarvan IsInRenderingThread , AMultiCameraManager::Tick(float DeltaTime)"));
	//}
	//if (IsInRHIThread())
	//{
	//	UE_LOG(LogTemp, Warning, TEXT("jarvan IsInRHIThread , AMultiCameraManager::Tick(float DeltaTime)"));
	//}
	 //TMap<int, test*> TT;
	 //for(int i = 0 ; i < 3 ; i++)
	 //{
	 //	auto t = new test(i, i, i);
	 //	TT.Add(i, t);
	 //}

	 //for(auto &t:TT)
	 //{
	 //	UE_LOG(LogTemp, Warning, TEXT("Jarvan in TMap TT, key %d, value %p, value add %p,  %d %d %d "), t.Key, t.Value, &t.Value, t.Value->A, t.Value->B, t.Value->C);
	 //}
	 //TMap<int, test*>TT2 = TT;
	 //for (auto &t : TT2)
	 //{
		// UE_LOG(LogTemp, Warning, TEXT("Jarvan in TMap TT2, key %d, value %p, value add %p,  %d %d %d "), t.Key, t.Value, &t.Value, t.Value->A, t.Value->B, t.Value->C);
	 //}
	 //TT.FindAndRemoveChecked(0);
	 //for (auto &t : TT)
	 //{
	 //	UE_LOG(LogTemp, Warning, TEXT("Jarvan in TMap TT, key %d, value %p, value add %p,  %d %d %d "), t.Key, t.Value, &t.Value, t.Value->A, t.Value->B, t.Value->C);
	 //}
	 //for (auto &t : TT2)
	 //{
		// UE_LOG(LogTemp, Warning, TEXT("Jarvan in TMap TT2, key %d, value %p, value add %p,  %d %d %d "), t.Key, t.Value, &t.Value, t.Value->A, t.Value->B, t.Value->C);
	 //}
	 //TT.Empty();
	 //UE_LOG(LogTemp, Warning, TEXT("Jarvan in TMap TT , size %d "),TT.Num());
	 //UE_LOG(LogTemp, Warning, TEXT("Jarvan in TMap TT2 , size %d "), TT2.Num());

}

void AMultiCameraManager::SetCameraDefaultOverrides()
{
	if(CaptureComponent2DMulti != nullptr)
	{
		auto &PostProcessSettings = CaptureComponent2DMulti->PostProcessSettings;

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
}

void AMultiCameraManager::ConfigureShowFlags()
{
	FEngineShowFlags &ShowFlags = CaptureComponent2DMulti->ShowFlags;
	bool bPostProcessing = bEnablePostProcessingEffects;

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
