// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma de Barcelona (UAB). This work is licensed under the terms of the MIT license. For a copy, see <https://opensource.org/licenses/MIT>.

#pragma once

#include "CoreMinimal.h"
#include "Sensor/Sensor.h"
#include "FisheyeCameraCS4.generated.h"


#if PLATFORM_WINDOWS
#  define CARLA_WITH_VULKAN_SUPPORT 1
#else
#  define CARLA_WITH_VULKAN_SUPPORT 1
#endif

class FPixelReader;
class UTextureRenderTarget2D;
class USceneCaptureComponent2D;
class UFisheyeCS4CameraRendering;

UCLASS()
class CARLA_API AFisheyeCameraCS4 : public ASensor
{
	GENERATED_BODY()

    friend class FPixelReader;
public:

    AFisheyeCameraCS4(const FObjectInitializer &ObjectInitializer);

    //GetSensorDefinition()和Set(const FActorDescription &ActorDescription)是一对 , 一个获取参数 , 一个是设置参数
    //this is going to be used to create a new blueprint in our blueprint library, users can use this blueprint to configure and spawn this sensor. 
    //确定了在python端默认的camera有那些参数并设置其默认参数
    static FActorDefinition GetSensorDefinition();

    //TBD : set function is called before UE4's BeginPlay
    //Immediately after the sensor is created, the Set function is called with the parameters that the user requested
    //在创建camera后调用,设置其各项参数
    void Set(const FActorDescription &ActorDescription) override;

    void SetImageWidth(int W);
    int GetImageWidth() const
    {
        return ImageWidth;
    }

    void SetImageHeight(int H);
    int GetImageHeight() const
    {
        return ImageHeight;
    }

    void SetFOV(float fov);
    float GetFOV() const
    {
        return FOV;
    }
    float GetFOVAngle() const
    {
        return FOV;
    }

    void Setd1(float d_1);
    float Getd1() const
    {
        return d1;
    }
    void Setd2(float d_2);
    float Getd2() const
    {
        return d2;
    }
    void Setd3(float d_3);
    float Getd3() const
    {
        return d3;
    }
    void Setd4(float d_4);
    float Getd4() const
    {
        return d4;
    }

    void Setfx(float f_x);
    float Getfx() const
    {
        return fx;
    }

    void Setfy(float f_y);
    float Getfy() const
    {
        return fy;
    }

    void Setcx(float c_x);
    float Getcx() const
    {
        return cx;
    }

    void Setcy(float c_y);
    float Getcy() const
    {
        return cy;
    }

    void SetSnitchNum(int Num);

    int GetSnitchNum() const
    {
        return SnitchNum;
    }

    void SetProjectionModel(int Model);

    int GetProjectionModel() const
    {
        return ProjectionModel;
    }

    UFUNCTION(BlueprintCallable)
    void EnablePostProcessingEffects(bool Enable = true)
    {
        bEnablePostProcessingEffects = Enable;
    }

    UFUNCTION(BlueprintCallable)
        bool ArePostProcessingEffectsEnabled() const
    {
        return bEnablePostProcessingEffects;
    }

    UFUNCTION(BlueprintCallable)
    void SetTargetGamma(float InTargetGamma)
    {
        TargetGamma = InTargetGamma;
    }

    UFUNCTION(BlueprintCallable)
    float GetTargetGamma() const
    {
        return  TargetGamma;
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

    ///// Use for debugging purposes only.
    //UFUNCTION(BlueprintCallable)
    //bool ReadPixels(TArray<FColor> &BitMap) const
    //{
    //    check(CaptureRenderTarget != nullptr);
    //    return FPixelReader::WritePixelsToArray(*CaptureRenderTarget, BitMap);
    //}

    ///// Use for debugging purposes only.
    //UFUNCTION(BlueprintCallable)
    //void SaveCaptureToDisk(const FString &FilePath) const
    //{
    //    check(CaptureRenderTarget != nullptr);
    //    FPixelReader::SavePixelsToDisk(*CaptureRenderTarget, FilePath);
    //}

    void ScreenshotToImage2D(const FString& InImagePath, UTextureRenderTarget2D* TextureTarget);

    void ColorToImage(const FString& InImagePath, TArray<FColor> InColor, int32 InWidth, int32 InHeight);

    void SendFisheyeCameraCSPixelsInRenderThread(AFisheyeCameraCS4 &Sensor);

    void WriteFisheyeCameraCSPixelsToBuffer(
        carla::Buffer &Buffer,
        uint32 Offset,
        AFisheyeCameraCS4 &Sensor,
        FRHICommandListImmediate &InRHICmdList);

protected:

    void BeginPlay() override;

    void Tick(float DeltaTime) override;

    void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    //void SetUpSceneCaptureComponent(USceneCaptureComponent2D &SceneCapture);

private:

    /// Image width in pixels.
    UPROPERTY(EditAnywhere)
    int ImageWidth = 1080u;

    UPROPERTY(EditAnywhere)
    int ImageHeight = 1080u;

    int SampleTextureWidth = 1080u;

    float Radius = 540.0;

    int SampleNum = 4;

    float SampleDist;

    /// Whether to render the post-processing effects present in the scene.
    UPROPERTY(EditAnywhere)
    bool bEnablePostProcessingEffects = true;

    UPROPERTY(EditAnywhere)
    float TargetGamma = 2.2f;

    //投影模型
    //0 : perspective
    //1 : stereographic
    //2 : equidistance
    //3 : equisolid
    //4 : orthogonal
    int ProjectionModel = 0;

    UPROPERTY(EditAnywhere)
    float FOV = 180.0f;
    UPROPERTY(EditAnywhere)
    float d1 = -0.1666665;
    UPROPERTY(EditAnywhere)
    float d2 = 0.0083330;
    UPROPERTY(EditAnywhere)
    float d3 = -0.0001980;
    UPROPERTY(EditAnywhere)
    float d4 = 0.00000260;
    UPROPERTY(EditAnywhere)
    float fx = 540.0;
    UPROPERTY(EditAnywhere)
    float fy = 540.0;
    UPROPERTY(EditAnywhere)
    float cx = 540.0;
    UPROPERTY(EditAnywhere)
    float cy = 540.0;

    int MipLevel = 6;

    UPROPERTY(EditAnywhere)
    TArray<UTextureRenderTarget2D*> CaptureRenderTarget;

    /// Scene capture component.
    UPROPERTY(EditAnywhere)
    TArray<USceneCaptureComponent2D*> CaptureComponent2D;

    UPROPERTY(EditAnywhere)
    UTextureRenderTarget2D* FishEyeTexture;

    UPROPERTY(EditAnywhere)
    UTextureRenderTarget2D* FishEyeTextureLDR;

    UPROPERTY(EditAnywhere)
    TArray<UTextureRenderTarget2D*> MipBloomRenderTarget;

    UPROPERTY(EditAnywhere)
    UFisheyeCS4CameraRendering* FisheyeCS4CameraRenderingPtr;

    UPROPERTY(EditAnywhere)
    int SnitchNum = 5;
};
