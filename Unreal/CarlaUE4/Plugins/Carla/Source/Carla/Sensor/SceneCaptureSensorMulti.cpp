// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma de Barcelona (UAB). This work is licensed under the terms of the MIT license. For a copy, see <https://opensource.org/licenses/MIT>.

#include "Carla.h"
#include "Carla/Sensor/SceneCaptureSensor.h"
#include "Carla/Game/CarlaStatics.h"
#include "Components/DrawFrustumComponent.h"
#include "Engine/Classes/Engine/Scene.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneCaptureComponent2DMulti.h"
#include "Carla/Sensor/SceneCaptureSensorMulti.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HighResScreenshot.h"
#include "ContentStreaming.h"
#include "ImageUtils.h"
#include "ModuleManager.h"
#include "Actor/ActorBlueprintFunctionLibrary.h"
#include "Sensor/PixelsSplitRunnable.h"
#include "FileHelper.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"

uint32 ASceneCaptureSensorMulti::IDGenerator = 0u;
uint32 ASceneCaptureSensorMulti::Count = 0u;

// =============================================================================
// -- ASceneCaptureSensorMulti ------------------------------------------------------
// =============================================================================


ASceneCaptureSensorMulti::ASceneCaptureSensorMulti(const FObjectInitializer &ObjectInitializer)
: Super(ObjectInitializer)
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PrePhysics;

	ID = IDGenerator;
	IDGenerator++;
	Count++;
}

FActorDefinition ASceneCaptureSensorMulti::GetSensorDefinition()
{
	constexpr bool bEnableModifyingPostProcessEffects = true;
	return UActorBlueprintFunctionLibrary::MakeCameraDefinition(
		TEXT("multi"),
		bEnableModifyingPostProcessEffects);
}

void ASceneCaptureSensorMulti::Set(const FActorDescription &Description)
{
	Super::Set(Description);
	FCameraAttribute CameraAttr;
	CameraAttr.SceneComponent = this->RootComponent;
	CameraAttr.FOVAngle = 90.0f;
	CameraAttr.OrthoWidth = 512;
	CameraAttr.ProjectionType = ECameraProjectionMode::Perspective;
	CameraAttr.bUseCustomProjectionMatrix = false;
	CameraAttr.CustomProjectionMatrix.SetIdentity();
	CameraAttr.ClipPlaneNormal = FVector(0, 0, 1);
	CameraAttr.bOverride_CustomNearClippingPlane = false;
	CameraAttr.SizeX = 800;
	CameraAttr.SizeY = 600;
	CaptureComponent2DMulti->AddCamera(ID, CameraAttr);
	UActorBlueprintFunctionLibrary::SetCamera(Description, this);
}

void ASceneCaptureSensorMulti::BeginPlay(){
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
  ThreadName = FString("SplitThreadNO.") + FString::FromInt(ID);
  PixelsSplitRunnable = FPixelsSplitRunnable::JoyInit(ThreadName);
}

void ASceneCaptureSensorMulti::Tick(float DeltaTime){
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

    //test for save image on disk
    {
        FDateTime Time = FDateTime::Now();
        int64 Timestamp = Time.ToUnixTimestamp();
        FString TimestampStr = FString::FromInt(Timestamp);
        FString SaveFileName = FPaths::ProjectSavedDir();
        SaveFileName.Append(FString("ASceneCaptureSensorMulti"));
        SaveFileName.Append(TimestampStr);
        SaveFileName.Append(".jpg");
        ScreenshotToImage2D(SaveFileName);
    }
};

void ASceneCaptureSensorMulti::EndPlay(const EEndPlayReason::Type EndPlayReason){
    Super::EndPlay(EndPlayReason);
	Count--;
	CaptureComponent2DMulti->CameraAttributeMap.FindAndRemoveChecked(ID);
	CameraManager->MultiCameras.FindAndRemoveChecked(ID);
};

void ASceneCaptureSensorMulti::SetUpSceneCaptureComponent(USceneCaptureComponent2DMulti &SceneCapture) {
	// SetUpSceneCaptureComponent是个虚函数 , 在ASceneCaptureSensor中里面什么也没有
	// 只有ASceneCaptureSensor的子类 , 比如AShaderBasedSensor , 才会重写SetUpSceneCaptureComponent函数 , 
	// 然后在这里执行
}


void ASceneCaptureSensorMulti::SetImageSize(uint32 InWidth, uint32 InHeight)
{
	CaptureComponent2DMulti->CameraAttributeMap[ID].SizeX = InWidth;
	CaptureComponent2DMulti->CameraAttributeMap[ID].SizeY = InHeight;
	Width = InWidth;
	Height = InHeight;
}

void ASceneCaptureSensorMulti::SetFOVAngle(const float FOVAngle)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->CameraAttributeMap[ID].FOVAngle = FOVAngle;
	FOV = FOVAngle;
}

float ASceneCaptureSensorMulti::GetFOVAngle() const
{
	check(CaptureComponent2DMulti != nullptr);
	if (this->CaptureComponent2DMulti && this->CaptureComponent2DMulti->CameraAttributeMap.Contains(ID)) {
		return CaptureComponent2DMulti->CameraAttributeMap[ID].FOVAngle;
	}
	return FOV;
}

void ASceneCaptureSensorMulti::ScreenshotToImage2D(const FString& InImagePath)
{

    if (CaptureComponent2DMulti && CaptureComponent2DMulti->TextureTarget)
    {
        //GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, FString::FromInt(CaptureComponent2D[i]->TextureTarget->SizeX));
        //GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, FString::FromInt(CaptureComponent2D[i]->TextureTarget->SizeY));

        FTextureRenderTargetResource* TextureRenderTargetResource = CaptureComponent2DMulti->TextureTarget->GameThread_GetRenderTargetResource();
        int32 Width = CaptureComponent2DMulti->TextureTarget->SizeX;
        int32 Height = CaptureComponent2DMulti->TextureTarget->SizeY;
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
        UE_LOG(LogTemp, Warning, TEXT("NO AFisheyeCameraMulti CaptureComponent2DMulti->TextureTarget"));
    }
}


//void AFisheyeCameraMulti::ScreenshotToImage2D(const FString& InImagePath, int i)
//{
//
//    if (CaptureComponent2DMulti && CaptureComponent2DMulti->TextureTarget)
//    {
//        //GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, FString::FromInt(CaptureComponent2D[i]->TextureTarget->SizeX));
//        //GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, FString::FromInt(CaptureComponent2D[i]->TextureTarget->SizeY));
//
//        FTextureRenderTargetResource* TextureRenderTargetResource = CaptureComponent2DMulti->TextureTarget->GameThread_GetRenderTargetResource();
//        int32 Width = CaptureComponent2DMulti->TextureTarget->SizeX;
//        int32 Height = CaptureComponent2DMulti->TextureTarget->SizeY;
//        //FIntRect Pos = CaptureComponent2DMulti->CameraAttributeMap[ID].PosInRenderTarget;
//        //uint32 Width = Pos.Max.X - Pos.Min.X;
//        //uint32 Height = Pos.Max.Y - Pos.Min.Y;
//        if (Width > 0 && Height > 0) {
//            TArray<FColor> OutData;
//            TextureRenderTargetResource->ReadPixels(OutData, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), FIntRect(0, 0, Width, Height));
//            //UE_LOG(LogTemp, Warning, TEXT("ID is %d , pos in render target is %d "), OutData.Num(), OutData.GetAllocatedSize()); 
//            //CaptureComponent2DMulti->CameraAttributeMap[ID].PosInRenderTarget
//            //UE_LOG(LogTemp, Warning, TEXT("after readpixels , TArray size is %d , data size is %d "), OutData.Num(), OutData.GetAllocatedSize());
//            ColorToImage(InImagePath, OutData, Width, Height);
//            //UE_LOG(LogTemp, Warning, TEXT("width = %d , height = %d , TArray size is %d"), Width, Height, OutData.Num());
//        }
//    }
//    else {
//        UE_LOG(LogTemp, Warning, TEXT("NO CaptureComponent2D_No.%d->TextureTarget"), i);
//    }
//}

void ASceneCaptureSensorMulti::ColorToImage(const FString& InImagePath, TArray<FColor> InColor, int32 InWidth, int32 InHeight)
{
    IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked <IImageWrapperModule>("ImageWrapper");
    FString Ex = FPaths::GetExtension(InImagePath);
    UE_LOG(LogTemp, Warning, TEXT("InWidth:%d,InHeight:%d,InImagePath:%s"), InWidth, InHeight, TCHAR_TO_UTF8(*InImagePath));

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



































void ASceneCaptureSensorMulti::SetExposureMethod(EAutoExposureMethod Method)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.AutoExposureMethod = Method;
}

EAutoExposureMethod ASceneCaptureSensorMulti::GetExposureMethod() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.AutoExposureMethod;
}

void ASceneCaptureSensorMulti::SetExposureCompensation(float Compensation)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.AutoExposureBias = Compensation;
}

float ASceneCaptureSensorMulti::GetExposureCompensation() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.AutoExposureBias;
}

void ASceneCaptureSensorMulti::SetShutterSpeed(float Speed)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.CameraShutterSpeed = Speed;
}

float ASceneCaptureSensorMulti::GetShutterSpeed() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.CameraShutterSpeed;
}

void ASceneCaptureSensorMulti::SetISO(float ISO)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.CameraISO = ISO;
}

float ASceneCaptureSensorMulti::GetISO() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.CameraISO;
}

void ASceneCaptureSensorMulti::SetAperture(float Aperture)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldFstop = Aperture;
}

float ASceneCaptureSensorMulti::GetAperture() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldFstop;
}

void ASceneCaptureSensorMulti::SetFocalDistance(float Distance)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldFocalDistance = Distance;
}

float ASceneCaptureSensorMulti::GetFocalDistance() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldFocalDistance;
}

void ASceneCaptureSensorMulti::SetDepthBlurAmount(float Amount)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldDepthBlurAmount = Amount;
}

float ASceneCaptureSensorMulti::GetDepthBlurAmount() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldDepthBlurAmount;
}

void ASceneCaptureSensorMulti::SetDepthBlurRadius(float Radius)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldDepthBlurRadius = Radius;
}

float ASceneCaptureSensorMulti::GetDepthBlurRadius() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldDepthBlurRadius;
}

void ASceneCaptureSensorMulti::SetDepthOfFieldMinFstop(float MinFstop)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldMinFstop = MinFstop;
}

float ASceneCaptureSensorMulti::GetDepthOfFieldMinFstop() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldMinFstop;
}

void ASceneCaptureSensorMulti::SetBladeCount(int Count)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldBladeCount = Count;
}

int ASceneCaptureSensorMulti::GetBladeCount() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldBladeCount;
}

void ASceneCaptureSensorMulti::SetFilmSlope(float Slope)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.FilmSlope = Slope;
}

float ASceneCaptureSensorMulti::GetFilmSlope() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.FilmSlope;
}

void ASceneCaptureSensorMulti::SetFilmToe(float Toe)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.FilmToe = Toe; // FilmToeAmount?
}

float ASceneCaptureSensorMulti::GetFilmToe() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.FilmToe;
}

void ASceneCaptureSensorMulti::SetFilmShoulder(float Shoulder)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.FilmShoulder = Shoulder;
}

float ASceneCaptureSensorMulti::GetFilmShoulder() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.FilmShoulder;
}

void ASceneCaptureSensorMulti::SetFilmBlackClip(float BlackClip)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.FilmBlackClip = BlackClip;
}

float ASceneCaptureSensorMulti::GetFilmBlackClip() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.FilmBlackClip;
}

void ASceneCaptureSensorMulti::SetFilmWhiteClip(float WhiteClip)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.FilmWhiteClip = WhiteClip;
}

float ASceneCaptureSensorMulti::GetFilmWhiteClip() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.FilmWhiteClip;
}

void ASceneCaptureSensorMulti::SetExposureMinBrightness(float Brightness)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.AutoExposureMinBrightness = Brightness;
}

float ASceneCaptureSensorMulti::GetExposureMinBrightness() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.AutoExposureMinBrightness;
}

void ASceneCaptureSensorMulti::SetExposureMaxBrightness(float Brightness)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.AutoExposureMaxBrightness = Brightness;
}

float ASceneCaptureSensorMulti::GetExposureMaxBrightness() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.AutoExposureMaxBrightness;
}

void ASceneCaptureSensorMulti::SetExposureSpeedDown(float Speed)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.AutoExposureSpeedDown = Speed;
}

float ASceneCaptureSensorMulti::GetExposureSpeedDown() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.AutoExposureSpeedDown;
}

void ASceneCaptureSensorMulti::SetExposureSpeedUp(float Speed)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.AutoExposureSpeedUp = Speed;
}

float ASceneCaptureSensorMulti::GetExposureSpeedUp() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.AutoExposureSpeedUp;
}

void ASceneCaptureSensorMulti::SetExposureCalibrationConstant(float Constant)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.AutoExposureCalibrationConstant = Constant;
}

float ASceneCaptureSensorMulti::GetExposureCalibrationConstant() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.AutoExposureCalibrationConstant;
}

void ASceneCaptureSensorMulti::SetMotionBlurIntensity(float Intensity)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.MotionBlurAmount = Intensity;
}

float ASceneCaptureSensorMulti::GetMotionBlurIntensity() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.MotionBlurAmount;
}

void ASceneCaptureSensorMulti::SetMotionBlurMaxDistortion(float MaxDistortion)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.MotionBlurMax = MaxDistortion;
}

float ASceneCaptureSensorMulti::GetMotionBlurMaxDistortion() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.MotionBlurMax;
}

void ASceneCaptureSensorMulti::SetMotionBlurMinObjectScreenSize(float ScreenSize)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.MotionBlurPerObjectSize = ScreenSize;
}

float ASceneCaptureSensorMulti::GetMotionBlurMinObjectScreenSize() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.MotionBlurPerObjectSize;
}

void ASceneCaptureSensorMulti::SetWhiteTemp(float Temp)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.WhiteTemp = Temp;
}

float ASceneCaptureSensorMulti::GetWhiteTemp() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.WhiteTemp;
}

void ASceneCaptureSensorMulti::SetWhiteTint(float Tint)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.WhiteTint = Tint;
}

float ASceneCaptureSensorMulti::GetWhiteTint() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.WhiteTint;
}

void ASceneCaptureSensorMulti::SetChromAberrIntensity(float Intensity)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.SceneFringeIntensity = Intensity;
}

float ASceneCaptureSensorMulti::GetChromAberrIntensity() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.SceneFringeIntensity;
}

void ASceneCaptureSensorMulti::SetChromAberrOffset(float Offset)
{
	check(CaptureComponent2DMulti != nullptr);
	CaptureComponent2DMulti->PostProcessSettings.ChromaticAberrationStartOffset = Offset;
}

float ASceneCaptureSensorMulti::GetChromAberrOffset() const
{
	check(CaptureComponent2DMulti != nullptr);
	return CaptureComponent2DMulti->PostProcessSettings.ChromaticAberrationStartOffset;
}