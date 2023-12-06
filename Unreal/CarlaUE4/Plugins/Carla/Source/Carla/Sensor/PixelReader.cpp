// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "Carla.h"
#include "Carla/Sensor/PixelReader.h"
#include "Carla/Sensor/SceneCaptureSensorMulti.h"
#include "ContentStreaming.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HighResScreenshot.h"
#include "Engine/World.h"
#include <iostream>
#include <chrono>
#include <ctime>    

#include "PixelsSplitRunnable.h"
#include "Runtime/ImageWriteQueue/Public/ImageWriteQueue.h"

// For now we only support Vulkan on Windows.
#if PLATFORM_WINDOWS
#  define CARLA_WITH_VULKAN_SUPPORT 1
#else
#  define CARLA_WITH_VULKAN_SUPPORT 1
#endif

// =============================================================================
// -- Local variables and types ------------------------------------------------
// =============================================================================

// RHILockTexture2D是加锁读取并返回Texture数组首地址 , 所以LockTexture在构造后就用Source存储地址 , 析构的时候解锁
struct LockTexture
{
  LockTexture(FRHITexture2D *InTexture, uint32 &Stride)
    : Texture(InTexture),
      Source(reinterpret_cast<const uint8 *>(
            RHILockTexture2D(Texture, 0, RLM_ReadOnly, Stride, false))) {}

  ~LockTexture()
  {
    RHIUnlockTexture2D(Texture, 0, false);
  }

  FRHITexture2D *Texture;

  const uint8 *Source;
};

// =============================================================================
// -- Static local functions ---------------------------------------------------
// =============================================================================

//#if CARLA_WITH_VULKAN_SUPPORT == 1

static void WritePixelsToBuffer_Vulkan(
    const UTextureRenderTarget2D &RenderTarget,
    carla::Buffer &Buffer,
    uint32 Offset,
    FRHICommandListImmediate &InRHICmdList)
{
  check(IsInRenderingThread());
  auto RenderResource =
      static_cast<const FTextureRenderTarget2DResource *>(RenderTarget.Resource);
  FTexture2DRHIRef Texture = RenderResource->GetRenderTargetTexture();
  if (!Texture)
  {
    UE_LOG(LogCarla, Error, TEXT("FPixelReader: UTextureRenderTarget2D missing render target texture"));
    return;
  }

  // NS: Extra copy here, don't know how to avoid it.
  TArray<FColor> Pixels;
  InRHICmdList.ReadSurfaceData(
      Texture,
      FIntRect(0, 0, RenderResource->GetSizeXY().X, RenderResource->GetSizeXY().Y),
      Pixels,
      FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX));

  Buffer.copy_from(Offset, Pixels);
}


static void WriteSplitPixelsToBuffer_Vulkan(
	const UTextureRenderTarget2D &RenderTarget,
	const FIntRect &PosInRenderTarget,
	carla::Buffer &Buffer,
	uint32 Offset,
	FRHICommandListImmediate &InRHICmdList)
{
	check(IsInRenderingThread());
	auto RenderResource =
		static_cast<const FTextureRenderTarget2DResource *>(RenderTarget.Resource);
	FTexture2DRHIRef Texture = RenderResource->GetRenderTargetTexture();
	if (!Texture)
	{
		UE_LOG(LogCarla, Error, TEXT("FPixelReader: UTextureRenderTarget2D missing render target texture"));
		return;
	}

	// NS: Extra copy here, don't know how to avoid it.
	TArray<FColor> Pixels;
	InRHICmdList.ReadSurfaceData(
		Texture,
		PosInRenderTarget,
		Pixels,
		FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX));

	Buffer.copy_from(Offset, Pixels);
}


//#endif // CARLA_WITH_VULKAN_SUPPORT

// =============================================================================
// -- FPixelReader -------------------------------------------------------------
// =============================================================================

bool FPixelReader::WritePixelsToArray(
    UTextureRenderTarget2D &RenderTarget,
    TArray<FColor> &BitMap)
{
  check(IsInGameThread());
  FTextureRenderTargetResource *RTResource =
      RenderTarget.GameThread_GetRenderTargetResource();
  if (RTResource == nullptr)
  {
    UE_LOG(LogCarla, Error, TEXT("FPixelReader: UTextureRenderTarget2D missing render target"));
    return false;
  }
  FReadSurfaceDataFlags ReadPixelFlags(RCM_UNorm);
  ReadPixelFlags.SetLinearToGamma(true);
  return RTResource->ReadPixels(BitMap, ReadPixelFlags);
}

bool FPixelReader::WritePixelsToArray(
	UTextureRenderTarget2D &RenderTarget,
	TArray<FColor> &BitMap,
	FIntRect &PosInRenderTarget)
{
	check(IsInGameThread());
	FTextureRenderTargetResource *RTResource =
		RenderTarget.GameThread_GetRenderTargetResource();
	if (RTResource == nullptr)
	{
		UE_LOG(LogCarla, Error, TEXT("FPixelReader: UTextureRenderTarget2D Multi missing render target"));
		return false;
	}
	FReadSurfaceDataFlags ReadPixelFlags(RCM_UNorm);
	ReadPixelFlags.SetLinearToGamma(true);
	return RTResource->ReadPixels(BitMap, ReadPixelFlags, PosInRenderTarget);
}

TUniquePtr<TImagePixelData<FColor>> FPixelReader::DumpPixels(
    UTextureRenderTarget2D &RenderTarget)
{
  const FIntPoint DestSize(RenderTarget.GetSurfaceWidth(), RenderTarget.GetSurfaceHeight());
  TUniquePtr<TImagePixelData<FColor>> PixelData = MakeUnique<TImagePixelData<FColor>>(DestSize);
  if (!WritePixelsToArray(RenderTarget, PixelData->Pixels))
  {
    return nullptr;
  }
  return PixelData;
}

TUniquePtr<TImagePixelData<FColor>> FPixelReader::DumpPixels(
	UTextureRenderTarget2D &RenderTarget,
	FIntRect &PosInRenderTarget)
{
	const FIntPoint DestSize(PosInRenderTarget.Width(), PosInRenderTarget.Height());
	TUniquePtr<TImagePixelData<FColor>> PixelData = MakeUnique<TImagePixelData<FColor>>(DestSize);
	if (!WritePixelsToArray(RenderTarget, PixelData->Pixels, PosInRenderTarget))
	{
		return nullptr;
	}
	return PixelData;
}

TFuture<bool> FPixelReader::SavePixelsToDisk(
    UTextureRenderTarget2D &RenderTarget,
    const FString &FilePath)
{
  return SavePixelsToDisk(DumpPixels(RenderTarget), FilePath);
}

TFuture<bool> FPixelReader::SavePixelsToDisk(
	UTextureRenderTarget2D &RenderTarget,
	const FString &FilePath,
	FIntRect &PosInRenderTarget)
{
	return SavePixelsToDisk(DumpPixels(RenderTarget, PosInRenderTarget), FilePath);
}

TFuture<bool> FPixelReader::SavePixelsToDisk(
    TUniquePtr<TImagePixelData<FColor>> PixelData,
    const FString &FilePath)
{
  TUniquePtr<FImageWriteTask> ImageTask = MakeUnique<FImageWriteTask>();
  ImageTask->PixelData = MoveTemp(PixelData);
  ImageTask->Filename = FilePath;
  ImageTask->Format = EImageFormat::PNG;
  ImageTask->CompressionQuality = (int32) EImageCompressionQuality::Default;
  ImageTask->bOverwriteFile = true;
  ImageTask->PixelPreProcessors.Add(TAsyncAlphaWrite<FColor>(255));

  FHighResScreenshotConfig &HighResScreenshotConfig = GetHighResScreenshotConfig();
  return HighResScreenshotConfig.ImageWriteQueue->Enqueue(MoveTemp(ImageTask));
}


void FPixelReader::WritePixelsToBuffer(
	UTextureRenderTarget2D &RenderTarget,
	carla::Buffer &Buffer,
	uint32 Offset,
	ASensor &Sensor,
	FRHICommandListImmediate &
#if CARLA_WITH_VULKAN_SUPPORT == 1
	InRHICmdList
#endif // CARLA_WITH_VULKAN_SUPPORT
)
{
	check(IsInRenderingThread());

	auto t1 = std::chrono::system_clock::now();

#if CARLA_WITH_VULKAN_SUPPORT == 1
	if (IsVulkanPlatform(GMaxRHIShaderPlatform))
	{
		UE_LOG(LogTemp, Warning, TEXT("Jarvan FPixelReader::WritePixelsToBuffer( IsVulkanPlatform"));
		WritePixelsToBuffer_Vulkan(RenderTarget, Buffer, Offset, InRHICmdList);
		return;
	}
#endif // CARLA_WITH_VULKAN_SUPPORT

	FRHITexture2D *Texture = RenderTarget.GetRenderTargetResource()->GetRenderTargetTexture();
	checkf(Texture != nullptr, TEXT("FPixelReader: UTextureRenderTarget2D missing render target texture"));

	const uint32 BytesPerPixel = 4u; // PF_R8G8B8A8
	const uint32 Width = Texture->GetSizeX();
	const uint32 Height = Texture->GetSizeY();
	const uint32 ExpectedStride = Width * BytesPerPixel;

	uint32 SrcStride;
	// 这个函数结束后Lock自动析构 , 就释放了RHILockTexture2D锁
	LockTexture Lock(Texture, SrcStride);

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
		const uint8 *SrcRow = Lock.Source;
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
		const uint8 *Source = Lock.Source;
		//这里会创建boost::asio::buffer里的const_buffer , 不可修改buffer里的数据
		Buffer.copy_from(Offset, Source, ExpectedStride * Height);
	}
	auto t3 = std::chrono::system_clock::now();
	std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;
	std::chrono::duration<double> elapsed_seconds_2 = t3 - t2;
	//  LockTexture Lock(Texture, SrcStride);耗时3ms
	UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time WritePixelsToBuffer , %.8lf, %.8lf, %.8lf"), elapsed_seconds_1.count(), elapsed_seconds_2.count());

}




void FPixelReader::WriteSplitPixelsToBuffer(
	UTextureRenderTarget2D &RenderTarget,
	FIntRect &PosInRenderTarget,
	carla::Buffer &Buffer,
	uint32 Offset,
	ASensor &Sensor,
	FRHICommandListImmediate &
#if CARLA_WITH_VULKAN_SUPPORT == 1
	InRHICmdList
#endif // CARLA_WITH_VULKAN_SUPPORT
	)
{
	check(IsInRenderingThread());

	auto start = std::chrono::system_clock::now();

#if CARLA_WITH_VULKAN_SUPPORT == 1
	if (IsVulkanPlatform(GMaxRHIShaderPlatform))
	{
		WriteSplitPixelsToBuffer_Vulkan(RenderTarget, PosInRenderTarget,Buffer, Offset, InRHICmdList);
		return;
	}
#endif // CARLA_WITH_VULKAN_SUPPORT
	FRHITexture2D *Texture = RenderTarget.GetRenderTargetResource()->GetRenderTargetTexture();
	checkf(Texture != nullptr, TEXT("FPixelReader::WriteSplitPixelsToBuffer():UTextureRenderTarget2D missing render target texture"));

	const uint32 BytesPerPixel = 4u; // PF_R8G8B8A8
	const uint32 Width = PosInRenderTarget.Width();
	const uint32 Height = PosInRenderTarget.Height();
	const uint32 ExpectedStride = Width * BytesPerPixel;

	uint32 SrcStride;
	// 这个函数结束后Lock自动析构 , 就释放了RHILockTexture2D锁
	// SrcStride : output to retrieve the textures row stride (pitch)
	// 自动加了读锁 , 允许一起读不能一起写
	LockTexture Lock(Texture, SrcStride);

	auto t2 = std::chrono::system_clock::now();

	Buffer.reset(Offset + ExpectedStride * Height);
	uint8 *DstRow = Buffer.begin() + Offset;
	const uint8 *SrcRow = Lock.Source + PosInRenderTarget.Min.Y * SrcStride + PosInRenderTarget.Min.X  * BytesPerPixel;
	for (uint32 Row = 0u; Row < Height; ++Row)
	{
		//每一次只从SrcRow拷贝ExpectedStride长度到DstRow , 说明ExpectedStride < SrcStride ,
		//而且说明SrcStride比ExpectedStride多出来的那一截是在尾部 , 可以抛弃
		FMemory::Memcpy(DstRow, SrcRow, ExpectedStride);
		SrcRow += SrcStride;
		DstRow += ExpectedStride;
	}
}






void FPixelReader::WriteAllSplitPixelsToBuffer(TMap<uint32, ASceneCaptureSensorMulti*> &Sensors, TMap<uint32, FAsyncDataStream*> Streams, TMap<uint32, carla::Buffer*> Buffers, FRHICommandListImmediate &InRHICmdList)
{
	check(IsInRenderingThread());

	auto t0 = std::chrono::system_clock::now();
	FRHITexture2D *Texture = nullptr;
	for (auto &Sensor : Sensors)
	{
		Texture = Sensor.Value->CaptureRenderTarget->GetRenderTargetResource()->GetRenderTargetTexture();
		checkf(Texture != nullptr, TEXT("FPixelReader::WriteAllSplitPixelsToBuffer():UTextureRenderTarget2D missing render target texture"));
		break;
	}
	if (Texture == nullptr || Texture->GetSizeX() == 0 || Texture->GetSizeY() == 0)
	{
		return;
	}

	auto t1 = std::chrono::system_clock::now();

	uint32 SrcStride;
	LockTexture Lock(Texture, SrcStride);
	
	uint32 MaxWidth = Texture->GetSizeX();
	uint32 MaxHeight = Texture->GetSizeY();
	auto t2 = std::chrono::system_clock::now();

	for (auto &Sensor : Sensors)
	{
		if(Streams.Contains(Sensor.Key) && Buffers.Contains(Sensor.Key))
		{
			Sensor.Value->PixelsSplitRunnable->Sensor = Sensor.Value;
			Sensor.Value->PixelsSplitRunnable->Stream = Streams[Sensor.Key];
			Sensor.Value->PixelsSplitRunnable->Buffer = Buffers[Sensor.Key];
			Sensor.Value->PixelsSplitRunnable->SrcStride = SrcStride;
			Sensor.Value->PixelsSplitRunnable->Texture = Texture;
			Sensor.Value->PixelsSplitRunnable->Source = Lock.Source;
			Sensor.Value->PixelsSplitRunnable->Suspend(Sensor.Value->ThreadName, false, false);
			//CopyAndSend(Sensor.Value, Streams[Sensor.Key], Buffers[Sensor.Key], SrcStride, Texture, Lock.Source);
		}
		//IStreamingManager::Get().AddViewInformation(
		//	Sensor->GetActorLocation(),
		//	Sensor->GetImageWidth(),
		//	Sensor->GetImageHeight() / FMath::Tan(Sensor->GetFOVAngle()));

	//	auto start = std::chrono::system_clock::now();

	//	const uint32 BytesPerPixel = 4u; // PF_R8G8B8A8
	//	const uint32 Width = Sensor.Value->GetImageWidth();
	//	const uint32 Height = Sensor.Value->GetImageHeight();
	//	const uint32 ExpectedStride = Width * BytesPerPixel;
	//	AllSensorStride += ExpectedStride;

	//	if (AllSensorStride > SrcStride)
	//	{
	//		UE_LOG(LogTemp, Warning, TEXT("Jarvan AllSensorStride > SrcStride"));
	//		continue;
	//	}
	//	FIntRect PosInRenderTarget = Sensor.Value->GetPosInRendertarget();

	//	auto Buffer = Buffers[Sensor.Key];
	//	uint32 Offset = carla::sensor::SensorRegistry::get<ASceneCaptureSensorMulti *>::type::header_offset;
	//	Buffer->reset(Offset + ExpectedStride * Height);
	//	uint8 *DstRow = Buffer->begin() + Offset;
	//	const uint8 *SrcRow = Lock.Source + PosInRenderTarget.Min.Y * SrcStride + PosInRenderTarget.Min.X  * BytesPerPixel;

	//	auto mid = std::chrono::system_clock::now();

	//	for (uint32 Row = 0u; Row < Height; ++Row)
	//	{
	//		FMemory::Memcpy(DstRow, SrcRow, ExpectedStride);
	//		SrcRow += SrcStride;
	//		DstRow += ExpectedStride;
	//	}

	//	if (Streams.Contains(Sensor.Key) && Buffer->size() > 0)
	//	{
	//		Streams[Sensor.Key]->Send(*Sensor.Value, std::move(*Buffer));
	//	}

	//	auto end = std::chrono::system_clock::now();

	//	std::chrono::duration<double> elapsed_seconds_1 = mid - start;
	//	std::chrono::duration<double> elapsed_seconds_2 = end - mid;
	//	UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time sensor No.%d copy , %.8lf, %.8lf"), Sensor.Key, elapsed_seconds_1.count(), elapsed_seconds_2.count());
	}

	auto t3 = std::chrono::system_clock::now();
	std::chrono::duration<double> elapsed_seconds_0 = t1 - t0;
	std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;
	std::chrono::duration<double> elapsed_seconds_2 = t3 - t2;
	//LockTexture Lock(Texture, SrcStride); 5ms
	//elapsed_seconds_2 : 3ms
	UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time WriteAllSplitPixelsToBuffer  , %.8lf, %.8lf, %.8lf"), 
        elapsed_seconds_0.count(), elapsed_seconds_1.count(), elapsed_seconds_2.count());

    //make sure all cameras' finished sending data
    bool bExit = true;
    while(bExit)
    {
        for (auto &Sensor : Sensors)
        {
            if (Streams.Contains(Sensor.Key) && Buffers.Contains(Sensor.Key))
            {
                Sensor.Value->PixelsSplitRunnable->Sensor = Sensor.Value;
                Sensor.Value->PixelsSplitRunnable->Stream = Streams[Sensor.Key];
                Sensor.Value->PixelsSplitRunnable->Buffer = Buffers[Sensor.Key];
                Sensor.Value->PixelsSplitRunnable->SrcStride = SrcStride;
                Sensor.Value->PixelsSplitRunnable->Texture = Texture;
                Sensor.Value->PixelsSplitRunnable->Source = Lock.Source;
                bExit &= Sensor.Value->PixelsSplitRunnable->IsThreadPause(Sensor.Value->ThreadName);
                //CopyAndSend(Sensor.Value, Streams[Sensor.Key], Buffers[Sensor.Key], SrcStride, Texture, Lock.Source);
            }
        }
    }
}

void FPixelReader::CopyAndSend(ASceneCaptureSensorMulti *Sensor, FAsyncDataStream *Stream, carla::Buffer *Buffer, uint32 SrcStride , FRHITexture2D *Texture, const uint8 *Source)
{
	//IStreamingManager::Get().AddViewInformation(
//	Sensor->GetActorLocation(),
//	Sensor->GetImageWidth(),
//	Sensor->GetImageHeight() / FMath::Tan(Sensor->GetFOVAngle()));
	FIntRect PosInRenderTarget = Sensor->GetPosInRendertarget();
	UE_LOG(LogTemp, Warning, TEXT("Jarvan Texture width %d height %d"), Texture->GetSizeX(), Texture->GetSizeY());
	UE_LOG(LogTemp, Warning, TEXT("Jarvan Pos min x %d y %d , max x %d y %d "), 
		PosInRenderTarget.Min.X, PosInRenderTarget.Min.Y, PosInRenderTarget.Max.X, PosInRenderTarget.Max.Y);

	if(!FIntRect(FIntPoint(0,0), (Texture->GetSizeXY())).Contains(PosInRenderTarget.Max))
	{
		UE_LOG(LogTemp, Warning, TEXT("Jarvan Sensor %d not in Texture!"), Sensor->ID);
		return;
	}

	auto start = std::chrono::system_clock::now();

	const uint32 BytesPerPixel = 4u; // PF_R8G8B8A8
	const uint32 Width = Sensor->GetImageWidth();
	const uint32 Height = Sensor->GetImageHeight();
	const uint32 ExpectedStride = Width * BytesPerPixel;

	uint32 Offset = carla::sensor::SensorRegistry::get<ASceneCaptureSensorMulti *>::type::header_offset;
	Buffer->reset(Offset + ExpectedStride * Height);
	uint8 *DstRow = Buffer->begin() + Offset;
	const uint8 *SrcRow = Source + PosInRenderTarget.Min.Y * SrcStride + PosInRenderTarget.Min.X  * BytesPerPixel;

	auto mid = std::chrono::system_clock::now();

	for (uint32 Row = 0u; Row < Height; ++Row)
	{
		FMemory::Memcpy(DstRow, SrcRow, ExpectedStride);
		SrcRow += SrcStride;
		DstRow += ExpectedStride;
	}

	if (Buffer->size() > 0)
	{
		Stream->Send(*Sensor, std::move(*Buffer));
	}

	auto end = std::chrono::system_clock::now();

	std::chrono::duration<double> elapsed_seconds_1 = mid - start;
	std::chrono::duration<double> elapsed_seconds_2 = end - mid;
	UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time sensor No.%d copy , %.8lf, %.8lf"), Sensor->ID, elapsed_seconds_1.count(), elapsed_seconds_2.count());
}



//void FPixelReader::WriteAllSplitPixelsToBuffer(TMap<uint32, ASceneCaptureSensorMulti*> &Sensors, TMap<uint32, FAsyncDataStream*> Streams, TMap<uint32, carla::Buffer*> Buffers, FRHICommandListImmediate &InRHICmdList)
//{
//	//WriteSplitPixelsToBuffer_Vulkan有额外的拷贝支出 , 性能低下 , 3个multi的时候这段代码用时10ms
//	//{
//	//	check(IsInRenderingThread());
//
//	//	auto t0 = std::chrono::system_clock::now();
//	//	UTextureRenderTarget2D *RenderTarget = nullptr;
//	//	for(auto &Sensor : Sensors)
//	//	{
//	//		RenderTarget = Sensor.Value->CaptureRenderTarget;
//	//		checkf(RenderTarget != nullptr, TEXT("FPixelReader::WriteAllSplitPixelsToBuffer():UTextureRenderTarget2D missing !"));
//	//		break;
//	//	}
//	//	uint32 Offset = carla::sensor::SensorRegistry::get<ASceneCaptureSensorMulti *>::type::header_offset;
//	//	for (auto &Sensor : Sensors)
//	//	{
//	//		WriteSplitPixelsToBuffer_Vulkan(*RenderTarget, Sensor.Value->GetPosInRendertarget(), *Buffers[Sensor.Key], Offset, InRHICmdList);
//	//	}
//	//	auto t1 = std::chrono::system_clock::now();
//	//	std::chrono::duration<double> elapsed_seconds_0 = t1 - t0;
//	//	UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time WriteAllSplitPixelsToBuffer  , %.8lf"), elapsed_seconds_0.count());
//	//}
//
//	check(IsInRenderingThread());
//
//	auto t0 = std::chrono::system_clock::now();
//	FRHITexture2D *Texture = nullptr;
//	for(auto &Sensor : Sensors)
//	{
//		Texture = Sensor.Value->CaptureRenderTarget->GetRenderTargetResource()->GetRenderTargetTexture();
//		checkf(Texture != nullptr, TEXT("FPixelReader::WriteAllSplitPixelsToBuffer():UTextureRenderTarget2D missing render target texture"));
//		break;
//	}
//	if(Texture == nullptr || Texture->GetSizeX() == 0 || Texture->GetSizeY() == 0)
//	{
//		return;
//	}
//
//	auto t1 = std::chrono::system_clock::now();
//
//	uint32 SrcStride;
//	LockTexture Lock(Texture, SrcStride);
//	uint32 AllSensorStride = 0u;
//
//	auto t2 = std::chrono::system_clock::now();
//
//	for(auto &Sensor : Sensors)
//	{
//		//IStreamingManager::Get().AddViewInformation(
//		//	Sensor->GetActorLocation(),
//		//	Sensor->GetImageWidth(),
//		//	Sensor->GetImageHeight() / FMath::Tan(Sensor->GetFOVAngle()));
//
//		auto start = std::chrono::system_clock::now();
//
//		const uint32 BytesPerPixel = 4u; // PF_R8G8B8A8
//		const uint32 Width = Sensor.Value->GetImageWidth();
//		const uint32 Height = Sensor.Value->GetImageHeight();
//		const uint32 ExpectedStride = Width * BytesPerPixel;
//		AllSensorStride += ExpectedStride;
//
//		if(AllSensorStride > SrcStride)
//		{
//			UE_LOG(LogTemp, Warning, TEXT("Jarvan AllSensorStride > SrcStride"));
//			continue;
//		}
//		FIntRect PosInRenderTarget = Sensor.Value->GetPosInRendertarget();
//
//		auto Buffer = Buffers[Sensor.Key];
//		uint32 Offset = carla::sensor::SensorRegistry::get<ASceneCaptureSensorMulti *>::type::header_offset;
//		Buffer->reset(Offset + ExpectedStride * Height);
//		uint8 *DstRow = Buffer->begin() + Offset;
//		const uint8 *SrcRow = Lock.Source + PosInRenderTarget.Min.Y * SrcStride + PosInRenderTarget.Min.X  * BytesPerPixel;
//
//		auto mid = std::chrono::system_clock::now();
//
//		for (uint32 Row = 0u; Row < Height; ++Row)
//		{
//			FMemory::Memcpy(DstRow, SrcRow, ExpectedStride);
//			SrcRow += SrcStride;
//			DstRow += ExpectedStride;
//		}
//
//		if (Streams.Contains(Sensor.Key) && Buffer->size() > 0)
//		{
//			Streams[Sensor.Key]->Send(*Sensor.Value, std::move(*Buffer));
//		}
//
//		auto end = std::chrono::system_clock::now();
//
//		std::chrono::duration<double> elapsed_seconds_1 = mid - start;
//		std::chrono::duration<double> elapsed_seconds_2 = end - mid;
//		UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time sensor No.%d copy , %.8lf, %.8lf"), Sensor.Key, elapsed_seconds_1.count(), elapsed_seconds_2.count());
//	}
//
//
//	//for (auto Sensor : Sensors)
//	//{
//	//	if (Streams.Contains(Sensor.Key) && Buffers.Contains(Sensor.Key) && Buffers[Sensor.Key]->size() > 0)
//	//	{
//	//		Streams[Sensor.Key]->Send(*Sensor.Value, std::move(*Buffers[Sensor.Key]));
//	//	}
//	//}
//
//	auto t3 = std::chrono::system_clock::now();
//	std::chrono::duration<double> elapsed_seconds_0 = t1 - t0;
//	std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;
//	std::chrono::duration<double> elapsed_seconds_2 = t3 - t2;
//	//LockTexture Lock(Texture, SrcStride); 5ms
//	//elapsed_seconds_2 : 3ms
//	UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time WriteAllSplitPixelsToBuffer  , %.8lf, %.8lf, %.8lf"), elapsed_seconds_0.count(),elapsed_seconds_1.count(), elapsed_seconds_2.count());
//
//}


void FPixelReader::SendAllSplitPixelsInRenderThread(TMap<uint32, ASceneCaptureSensorMulti*> &Sensors)
{
	if (Sensors.Num() < 1)
	{
		//UE_LOG(LogTemp, Warning, TEXT("Jarvan ASceneCaptureSensorMulti num < 1"));
		return;
	}
	//Stream执行在游戏线程 , Buffer创建在渲染线程
	// Enqueue a command in the render-thread that will write the image buffer to
	// the data stream. The stream is created in the capture thus executed in the
	//// game-thread.
	ENQUEUE_RENDER_COMMAND(FWriteAllSplitPixels_SendSplitPixelsInRenderThread)
	(
		[&Sensors](auto &InRHICmdList) mutable
		{
			auto t1 = std::chrono::system_clock::now();

			TMap<uint32, FAsyncDataStream*> Streams;
			TMap<uint32, carla::Buffer*> Buffers;
			for (auto &Sensor : Sensors)
			{
				auto Stream = Sensor.Value->GetDataStreamPtr(*Sensor.Value);
				Streams.Add(Sensor.Key, Stream);
				auto Buffer = new carla::Buffer;
				Buffers.Add(Sensor.Key, Buffer);
			}

			auto t2 = std::chrono::system_clock::now();

			WriteAllSplitPixelsToBuffer(Sensors, Streams, Buffers, InRHICmdList);

			auto t3 = std::chrono::system_clock::now();

			//for (auto Sensor : Sensors)
			//{
			//	if(Streams.Contains(Sensor.Key) && Buffers.Contains(Sensor.Key) && Buffers[Sensor.Key]->size() > 0)
			//	{
			//		Streams[Sensor.Key]->Send(*Sensor.Value, std::move(*Buffers[Sensor.Key]));
			//	}
			//}

			auto t4 = std::chrono::system_clock::now();

			Streams.Empty();
			Buffers.Empty();

			auto t5 = std::chrono::system_clock::now();

			std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;
			std::chrono::duration<double> elapsed_seconds_2 = t3 - t2;
			std::chrono::duration<double> elapsed_seconds_3 = t4 - t3;
			std::chrono::duration<double> elapsed_seconds_4 = t5 - t4;
			//WriteAllSplitPixelsToBuffer(Sensors, Streams, Buffers) , 7ms
			UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time SendAllSplitPixelsInRenderThread , %.8lf, %.8lf, %.8lf , %.8lf"), 
                elapsed_seconds_1.count(), elapsed_seconds_2.count(), elapsed_seconds_3.count(), elapsed_seconds_4.count());;
		}
	);
}