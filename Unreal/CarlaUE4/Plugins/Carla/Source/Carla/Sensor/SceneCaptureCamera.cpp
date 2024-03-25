// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "Carla.h"
#include "Carla/Sensor/SceneCaptureCamera.h"

FActorDefinition ASceneCaptureCamera::GetSensorDefinition()
{
  constexpr bool bEnableModifyingPostProcessEffects = true;
  return UActorBlueprintFunctionLibrary::MakeCameraDefinition(
      TEXT("rgb"),
      bEnableModifyingPostProcessEffects);
}

ASceneCaptureCamera::ASceneCaptureCamera(const FObjectInitializer &ObjectInitializer)
  : Super(ObjectInitializer)
{
  AddPostProcessingMaterial(
      TEXT("Material'/Carla/PostProcessingMaterials/PhysicLensDistortion.PhysicLensDistortion'"));
}

void ASceneCaptureCamera::Tick(float DeltaTime)
{
  Super::Tick(DeltaTime);
  //FPixelReader::SendPixelsInRenderThread(*this);
  SendSceneCaptureCameraPixelsInRenderThread(*this);
}



void ASceneCaptureCamera::SendSceneCaptureCameraPixelsInRenderThread(ASceneCaptureCamera &Sensor)
{
    check(Sensor.CaptureRenderTarget != nullptr);
    // Enqueue a command in the render-thread that will write the image buffer to
    // the data stream. The stream is created in the capture thus executed in the
    // game-thread.

    ENQUEUE_RENDER_COMMAND(FWriteSceneCaptureCameraPixelsToBuffer_SendPixelsInRenderThread)
        (
            [&Sensor, Stream = this->GetDataStream(Sensor)](auto &InRHICmdList) mutable
    {
        if (!Sensor.IsPendingKill())
        {

            auto t1 = std::chrono::system_clock::now();

            auto Buffer = Stream.PopBufferFromPool();

            auto t2 = std::chrono::system_clock::now();

            Sensor.WriteSceneCaptureCameraPixelsToBuffer(
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
            UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time SendSceneCaptureCameraPixelsInRenderThread  , %.8lf, %.8lf, %.8lf"), elapsed_seconds_1.count(), elapsed_seconds_2.count(), elapsed_seconds_3.count());
        }
    }
    );
}

void ASceneCaptureCamera::WriteSceneCaptureCameraPixelsToBuffer(
    carla::Buffer &Buffer,
    uint32 Offset,
    ASceneCaptureCamera &Sensor,
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

    FRHITexture2D *Texture = Sensor.CaptureRenderTarget->GetRenderTargetResource()->GetRenderTargetTexture();
    checkf(Texture != nullptr, TEXT("FPixelReader: UTextureRenderTarget2D missing render target texture"));

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
