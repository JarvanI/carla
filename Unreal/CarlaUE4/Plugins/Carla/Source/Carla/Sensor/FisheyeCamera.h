// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma de Barcelona (UAB). This work is licensed under the terms of the MIT license. For a copy, see <https://opensource.org/licenses/MIT>.

#pragma once

#include "CoreMinimal.h"
#include "Sensor/Sensor.h"
#include "FisheyeCamera.generated.h"

// For now we only support Vulkan on Windows.
#if PLATFORM_WINDOWS
#  define CARLA_WITH_VULKAN_SUPPORT 1
#else
#  define CARLA_WITH_VULKAN_SUPPORT 1
#endif

class FPixelReader;
class UTextureRenderTarget2D;
class USceneCaptureComponent2D;

struct IImageSpace
{
    int i;
    int j;

    IImageSpace(int i, int j) :i(i), j(j) {}
    void Set(int x, int y)
    {
        i = x;
        j = y;
    }
};

struct ITexel
{
    int ID;
    IImageSpace TexelPos;

    ITexel() :ID(-1), TexelPos(-1, -1) {}
    ITexel(int id, int i, int j) :ID(id), TexelPos(i, j) {}

    void Set(int id, int i, int j)
    {
        ID = id;
        TexelPos.i = i;
        TexelPos.j = j;
    }
};



struct StitchInfos
{
    //最后sample取样的texel image space坐标
    TArray<ITexel> SampleTexel;
    //系数
    TArray<float> k;

    StitchInfos() {}
};


struct SampleInfo
{
    //sample在成像面的image space浮点坐标 , 顺序是从左到右
    FVector2D FSampleInPixelImageSpace;
    //sample过原点逆向后与包围盒交点在locl space的3D坐标 , -1到1
    FVector FSampleOriginPanelLocalSpaceNormal;
    //sample过原点逆向后与panel的交点texel image space坐标 , 整型
    TArray<ITexel> ISampleOriginPanelImageSpace;

    SampleInfo() :FSampleInPixelImageSpace(FVector2D::ZeroVector) {}
    SampleInfo(float x, float y) :FSampleInPixelImageSpace(x, y) {}
};

struct ImagingPixel
{
    //Pixel下标
    int i;
    int j;
    //采样频率
    int n;
    TArray<SampleInfo> SampleInfosArray;
    //NxN个sample是否在image内 , 0表示不在 , 1表示在
    //不在image时 , sample颜色为黑色FColor(0, 0, 0)
    TArray<uint8> SampleInImage;

    ImagingPixel(int i, int j, int n) :i(i), j(j), n(n)
    {
        SampleInfosArray.Init(SampleInfo(), n*n);
        float d = 1 / (2 * n);
        for (int k = 0; k < n; k++)
        {
            for (int l = 0; l < n; l++)
            {
                SampleInfosArray[k * n + l].FSampleInPixelImageSpace.X = float(i) + d * (2 * k + 1);
                SampleInfosArray[k * n + l].FSampleInPixelImageSpace.Y = float(j) + d * (2 * l + 1);
            }
        }

        SampleInImage.Init(uint8(0), n*n);
    }
};



/**
 * 
 */
UCLASS()
class CARLA_API AFisheyeCamera : public ASensor
{
	GENERATED_BODY()
    
    friend class FPixelReader;
public:

    AFisheyeCamera(const FObjectInitializer &ObjectInitializer);

    //GetSensorDefinition()和Set(const FActorDescription &ActorDescription)是一对 , 一个获取参数 , 一个是设置参数
    //this is going to be used to create a new blueprint in our blueprint library, users can use this blueprint to configure and spawn this sensor. 
    //确定了在python端默认的camera有那些参数并设置其默认参数
    static FActorDefinition GetSensorDefinition();

    //TBD : set function is called before UE4's BeginPlay
    //Immediately after the sensor is created, the Set function is called with the parameters that the user requested
    //在创建camera后调用,设置其各项参数
    void Set(const FActorDescription &ActorDescription) override;

    void SetImageSize(int Width);

    int GetImageWidth() const
    {
        return ImageWidth;
    }

    int GetImageHeight() const
    {
        return ImageWidth;
    }

    float GetFOVAngle() const
    {
        return 90.0;
    }

    void SetSSAA(int Num);

    int GetSSAA() const
    {
        return SampleNum;
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

    void CalPixelsRelationship();

    bool IsPixelInCircle(int PixelX, int PixelY);

    FVector2D PixelCoord2ImageCoord(int PixelI, int PixelJ);

    FVector RayPlaneIntersection(const FVector& RayOrigin, const FVector& RayDirection, const FPlane& Plane, bool& WillIntersection);

    bool IsPointInCube(FVector Point);

    void GetFishEyePic(const FString& InImagePath);

    void ScreenshotToImage2D(const FString& InImagePath, int Index);

    void ColorToImage(const FString& InImagePath, TArray<FColor> InColor, int32 InWidth, int32 InHeight);

    void ColorToImage(const FString& InImagePath, TArray<uint8> InColor, int32 InWidth, int32 InHeight);

    TArray<FColor>* GetOutData(int CameraID);

    FColor CalAvgColor(TArray<FColor>& PixelColor);

    bool IsSampleInCircle(float i, float j);

    FVector2D LoclSpace2Panel(int PanelID, FVector IntersectPoint);

    void SendFisheyePixelsInRenderThread(AFisheyeCamera &Sensor);

    void WriteFisheyePixelsToBuffer(
        carla::Buffer &Buffer,
        uint32 Offset,
        AFisheyeCamera &Sensor,
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

    float Radius = 540.0;

    int SampleNum = 2;

    float SampleDist;

    /// Whether to render the post-processing effects present in the scene.
    UPROPERTY(EditAnywhere)
        bool bEnablePostProcessingEffects = true;

    UPROPERTY(EditAnywhere)
        float TargetGamma = 2.2f;

    /// Render target necessary for scene capture.
    UPROPERTY(EditAnywhere)
        UTextureRenderTarget2D *CaptureRenderTarget[5];

    /// Scene capture component.
    UPROPERTY(EditAnywhere)
        USceneCaptureComponent2D *CaptureComponent2D[5];

    UPROPERTY(EditAnywhere)
        UTextureRenderTarget2D* FishEyeTexture;

    //记录在image内的pixel. 在nxn的sample后只要有一个point在image内就记录
    TMap<int, ImagingPixel> ImagingPixels;
    //索引表 , 值为0对应下标的ImagingPixels说明该pixel不在成像面的圆内
    TArray<int> ImaginPixelsQuerry;

    TArray<FPlane> PlaneArray;

    TArray<FColor> OutDataFront;
    TArray<FColor> OutDataLeft;
    TArray<FColor> OutDataRight;
    TArray<FColor> OutDataTop;
    TArray<FColor> OutDataBottom;
    TArray<FColor> OutDataFishEye;
};
