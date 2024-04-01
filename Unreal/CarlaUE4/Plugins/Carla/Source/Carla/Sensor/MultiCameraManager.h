// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma de Barcelona (UAB). This work is licensed under the terms of the MIT license. For a copy, see <https://opensource.org/licenses/MIT>.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MultiCameraManager.generated.h"


class ASceneCaptureSensorMulti;
class AFisheyeCameraMulti;

//AMultiCameraManager相当于demo里面的AScreeShotMulti类
UCLASS()
class CARLA_API AMultiCameraManager : public AActor
{
    GENERATED_BODY()

        friend class FPixelReader;
    friend class FPixelsSplitRunnable;

public:
    // Sets default values for this actor's properties
    AMultiCameraManager();

    //USceneCaptureComponent2DMulti* GetCaptureComponent2DMulti();

    void SetCameraDefaultOverrides();

    void ConfigureShowFlags();

    void RenderFunc();

    void CreateNewThread();

    //void RunThread(const FString&ThreadName);

    ////~开启控制台输入:ShutdownThread "001" 结束一个名为001的线程
    //UFUNCTION(Exec)
    //void ShutdownThread(const FString& ThreadName);

    ////~开启控制台输入:Suspend "001" true或者false 挂起或者激活一个名为001的线程
    //UFUNCTION(Exec)
    //	void Suspend(const FString& ThreadName, bool bSuspend, bool bUseSuspend);

protected:
    // Called when the game starts or when spawned
    virtual void BeginPlay() override;

public:

    // Called every frame
    virtual void Tick(float DeltaTime) override;

    /// Whether to render the post-processing effects present in the scene.
    UPROPERTY(EditAnywhere)
        bool bEnablePostProcessingEffects = true;

    UPROPERTY(EditAnywhere)
        float TargetGamma = 2.2f;

    /// Render target necessary for scene capture.
    UPROPERTY(EditAnywhere)
        class UTextureRenderTarget2D* CaptureRenderTarget = nullptr;

    /// Scene capture component.
    UPROPERTY(EditAnywhere)
        class USceneCaptureComponent2DMulti* CaptureComponent2DMulti = nullptr;

    UPROPERTY(EditAnywhere)
        TMap<uint32, ASceneCaptureSensorMulti*> MultiCameras;

    int32 count = 0;

    FPixelsSplitRunnable* PixelsSplitRunnable;
};
