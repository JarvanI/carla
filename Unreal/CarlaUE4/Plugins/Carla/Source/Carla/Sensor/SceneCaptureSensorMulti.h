// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma de Barcelona (UAB). This work is licensed under the terms of the MIT license. For a copy, see <https://opensource.org/licenses/MIT>.

#pragma once

#include "MultiCameraManager.h"
#include "Carla/Sensor/Sensor.h"
#include "Components/SceneCaptureComponent2DMulti.h"
#include "Carla/Sensor/PixelReader.h"
#include "SceneCaptureSensorMulti.generated.h"

class USceneCaptureComponent2DMulti;
class FPixelReader;

UCLASS()
class CARLA_API ASceneCaptureSensorMulti : public ASensor
{
	GENERATED_BODY()

	friend class FPixelReader;
public:

	//void AddCamera();

	ASceneCaptureSensorMulti(const FObjectInitializer &ObjectInitializer);

	//GetSensorDefinition()和Set(const FActorDescription &ActorDescription)是一对 , 一个获取参数 , 一个是设置参数
	//this is going to be used to create a new blueprint in our blueprint library, users can use this blueprint to configure and spawn this sensor. 
	//确定了在python端默认的camera有那些参数并设置其默认参数
	static FActorDefinition GetSensorDefinition();

	//TBD : set function is called before UE4's BeginPlay
	//Immediately after the sensor is created, the Set function is called with the parameters that the user requested
	//在创建camera后调用,设置其各项参数
	void Set(const FActorDescription &ActorDescription) override;

	void SetImageSize(uint32 Width, uint32 Height);

	uint32 GetImageWidth() const
	{
		if (this->CaptureComponent2DMulti && this->CaptureComponent2DMulti->CameraAttributeMap.Contains(ID)) {
			return CaptureComponent2DMulti->CameraAttributeMap[ID].SizeX;
		}
		return Width;
	}

	uint32 GetImageHeight() const
	{
		if (this->CaptureComponent2DMulti && this->CaptureComponent2DMulti->CameraAttributeMap.Contains(ID)) {
			return CaptureComponent2DMulti->CameraAttributeMap[ID].SizeY;
		}
		return Height;
	}

	FIntRect& GetPosInRendertarget() const
	{
		return CaptureComponent2DMulti->CameraAttributeMap[ID].PosInRenderTarget;
	}

	UFUNCTION(BlueprintCallable)
	void EnablePostProcessingEffects(bool Enable = true)
	{
		CameraManager->bEnablePostProcessingEffects = Enable;
	}

	UFUNCTION(BlueprintCallable)
	bool ArePostProcessingEffectsEnabled() const
	{
		return CameraManager->bEnablePostProcessingEffects;
	}

	UFUNCTION(BlueprintCallable)
	void SetFOVAngle(float FOVAngle);

	UFUNCTION(BlueprintCallable)
	float GetFOVAngle() const;

	UFUNCTION(BlueprintCallable)
	void SetTargetGamma(float InTargetGamma)
	{
		CameraManager->TargetGamma = InTargetGamma;
	}

	UFUNCTION(BlueprintCallable)
	float GetTargetGamma() const
	{
		return  CameraManager->TargetGamma;
	}

	UFUNCTION(BlueprintCallable)
		void SetExposureMethod(EAutoExposureMethod Method);

	UFUNCTION(BlueprintCallable)
		EAutoExposureMethod GetExposureMethod() const;

	UFUNCTION(BlueprintCallable)
		void SetExposureCompensation(float Compensation);

	UFUNCTION(BlueprintCallable)
		float GetExposureCompensation() const;

	UFUNCTION(BlueprintCallable)
		void SetShutterSpeed(float Speed);

	UFUNCTION(BlueprintCallable)
		float GetShutterSpeed() const;

	UFUNCTION(BlueprintCallable)
		void SetISO(float ISO);

	UFUNCTION(BlueprintCallable)
		float GetISO() const;

	UFUNCTION(BlueprintCallable)
		void SetAperture(float Aperture);

	UFUNCTION(BlueprintCallable)
		float GetAperture() const;

	UFUNCTION(BlueprintCallable)
		void SetFocalDistance(float Distance);

	UFUNCTION(BlueprintCallable)
		float GetFocalDistance() const;

	UFUNCTION(BlueprintCallable)
		void SetDepthBlurAmount(float Amount);

	UFUNCTION(BlueprintCallable)
		float GetDepthBlurAmount() const;

	UFUNCTION(BlueprintCallable)
		void SetDepthBlurRadius(float Radius);

	UFUNCTION(BlueprintCallable)
		float GetDepthBlurRadius() const;

	UFUNCTION(BlueprintCallable)
		void SetBladeCount(int Count);

	UFUNCTION(BlueprintCallable)
		int GetBladeCount() const;

	UFUNCTION(BlueprintCallable)
		void SetDepthOfFieldMinFstop(float MinFstop);

	UFUNCTION(BlueprintCallable)
		float GetDepthOfFieldMinFstop() const;

	UFUNCTION(BlueprintCallable)
		void SetFilmSlope(float Slope);

	UFUNCTION(BlueprintCallable)
		float GetFilmSlope() const;

	UFUNCTION(BlueprintCallable)
		void SetFilmToe(float Toe);

	UFUNCTION(BlueprintCallable)
		float GetFilmToe() const;

	UFUNCTION(BlueprintCallable)
		void SetFilmShoulder(float Shoulder);

	UFUNCTION(BlueprintCallable)
		float GetFilmShoulder() const;

	UFUNCTION(BlueprintCallable)
		void SetFilmBlackClip(float BlackClip);

	UFUNCTION(BlueprintCallable)
		float GetFilmBlackClip() const;

	UFUNCTION(BlueprintCallable)
		void SetFilmWhiteClip(float WhiteClip);

	UFUNCTION(BlueprintCallable)
		float GetFilmWhiteClip() const;

	UFUNCTION(BlueprintCallable)
		void SetExposureMinBrightness(float Brightness);

	UFUNCTION(BlueprintCallable)
		float GetExposureMinBrightness() const;

	UFUNCTION(BlueprintCallable)
		void SetExposureMaxBrightness(float Brightness);

	UFUNCTION(BlueprintCallable)
		float GetExposureMaxBrightness() const;

	UFUNCTION(BlueprintCallable)
		void SetExposureSpeedDown(float Speed);

	UFUNCTION(BlueprintCallable)
		float GetExposureSpeedDown() const;

	UFUNCTION(BlueprintCallable)
		void SetExposureSpeedUp(float Speed);

	UFUNCTION(BlueprintCallable)
		float GetExposureSpeedUp() const;

	UFUNCTION(BlueprintCallable)
		void SetExposureCalibrationConstant(float Constant);

	UFUNCTION(BlueprintCallable)
		float GetExposureCalibrationConstant() const;

	UFUNCTION(BlueprintCallable)
		void SetMotionBlurIntensity(float Intensity);

	UFUNCTION(BlueprintCallable)
		float GetMotionBlurIntensity() const;

	UFUNCTION(BlueprintCallable)
		void SetMotionBlurMaxDistortion(float MaxDistortion);

	UFUNCTION(BlueprintCallable)
		float GetMotionBlurMaxDistortion() const;

	UFUNCTION(BlueprintCallable)
		void SetMotionBlurMinObjectScreenSize(float ScreenSize);

	UFUNCTION(BlueprintCallable)
		float GetMotionBlurMinObjectScreenSize() const;

	UFUNCTION(BlueprintCallable)
		void SetWhiteTemp(float Temp);

	UFUNCTION(BlueprintCallable)
		float GetWhiteTemp() const;

	UFUNCTION(BlueprintCallable)
		void SetWhiteTint(float Tint);

	UFUNCTION(BlueprintCallable)
		float GetWhiteTint() const;

	UFUNCTION(BlueprintCallable)
		void SetChromAberrIntensity(float Intensity);

	UFUNCTION(BlueprintCallable)
		float GetChromAberrIntensity() const;

	UFUNCTION(BlueprintCallable)
		void SetChromAberrOffset(float Offset);

	UFUNCTION(BlueprintCallable)
		float GetChromAberrOffset() const;

	/// Use for debugging purposes only.
	UFUNCTION(BlueprintCallable)
	bool ReadMultiCameraPixels(TArray<FColor> &BitMap) const
	{
		check(CaptureRenderTarget != nullptr);
		return FPixelReader::WritePixelsToArray(*CaptureRenderTarget, BitMap);
	}

	/// Use for debugging purposes only.
	UFUNCTION(BlueprintCallable)
	bool ReadPixels(TArray<FColor> &BitMap) const
	{
		check(CaptureRenderTarget != nullptr);
		return FPixelReader::WritePixelsToArray(*CaptureRenderTarget, BitMap, GetPosInRendertarget());
	}

	/// Use for debugging purposes only.
	UFUNCTION(BlueprintCallable)
	void SaveMultiCameraCaptureToDisk(const FString &FilePath) const
	{
		check(CaptureRenderTarget != nullptr);
		FPixelReader::SavePixelsToDisk(*CaptureRenderTarget, FilePath);
	}

	UFUNCTION(BlueprintCallable)
	void SaveCaptureToDisk(const FString &FilePath) const
	{
		check(CaptureRenderTarget != nullptr);
		FPixelReader::SavePixelsToDisk(*CaptureRenderTarget, FilePath, GetPosInRendertarget());
	}

    void ScreenshotToImage2D(const FString& InImagePath);

    void ColorToImage(const FString& InImagePath, TArray<FColor> InColor, int32 InWidth, int32 InHeight);

protected:

	void BeginPlay() override;

	void Tick(float DeltaTime) override;

	void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	void SetUpSceneCaptureComponent(USceneCaptureComponent2DMulti &SceneCapture);

public:

	UPROPERTY(VisibleAnywhere)
	uint32 ID;

	UPROPERTY(VisibleAnywhere)
		uint32 Width;

	UPROPERTY(VisibleAnywhere)
		uint32 Height;

	UPROPERTY(VisibleAnywhere)
		float FOV;

	/// Render target necessary for scene capture.
	UPROPERTY(EditAnywhere)
	class UTextureRenderTarget2D* CaptureRenderTarget = nullptr;

	/// Scene capture component.
	UPROPERTY(VisibleAnywhere)
	class USceneCaptureComponent2DMulti* CaptureComponent2DMulti = nullptr;

	UPROPERTY(VisibleAnywhere)
	class AMultiCameraManager* CameraManager = nullptr;

	static uint32 IDGenerator;

	static uint32 Count;

	FPixelsSplitRunnable* PixelsSplitRunnable;

	FString ThreadName;
};
