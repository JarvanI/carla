// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#pragma once

#include "CoreGlobals.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Runtime/ImageWriteQueue/Public/ImagePixelData.h"
#include "Carla/Sensor/Sensor.h"
#include <compiler/disable-ue4-macros.h>
#include <carla/Buffer.h>
#include <carla/sensor/SensorRegistry.h>
#include <compiler/enable-ue4-macros.h>

// =============================================================================
// -- FPixelReader -------------------------------------------------------------
// =============================================================================

/// Utils for reading pixels from UTextureRenderTarget2D.
///
/// @todo This class only supports PF_R8G8B8A8 format.
class FPixelReader
{
public:

  /// Copy the pixels in @a RenderTarget into @a BitMap.
  ///
  /// @pre To be called from game-thread.
  static bool WritePixelsToArray(
      UTextureRenderTarget2D &RenderTarget,
      TArray<FColor> &BitMap);

  static bool WritePixelsToArray(
	  UTextureRenderTarget2D &RenderTarget,
	  TArray<FColor> &BitMap,
	  FIntRect &PosInRenderTarget);

  /// Dump the pixels in @a RenderTarget.
  ///
  /// @pre To be called from game-thread.
  static TUniquePtr<TImagePixelData<FColor>> DumpPixels(
      UTextureRenderTarget2D &RenderTarget);

  static TUniquePtr<TImagePixelData<FColor>> DumpPixels(
	  UTextureRenderTarget2D &RenderTarget,
	  FIntRect &PosInRenderTarget);
  /// Asynchronously save the pixels in @a RenderTarget to disk.
  ///
  /// @pre To be called from game-thread.
  static TFuture<bool> SavePixelsToDisk(
      UTextureRenderTarget2D &RenderTarget,
      const FString &FilePath);

  static TFuture<bool> SavePixelsToDisk(
	  UTextureRenderTarget2D &RenderTarget,
	  const FString &FilePath,
	  FIntRect &PosInRenderTarget);

  /// Asynchronously save the pixels in @a PixelData to disk.
  ///
  /// @pre To be called from game-thread.
  static TFuture<bool> SavePixelsToDisk(
      TUniquePtr<TImagePixelData<FColor>> PixelData,
      const FString &FilePath);

  /// Convenience function to enqueue a render command that sends the pixels
  /// down the @a Sensor's data stream. It expects a sensor derived from
  /// ASceneCaptureSensor or compatible.
  ///
  /// Note that the serializer needs to define a "header_offset" that it's
  /// allocated in front of the buffer.
  ///
  /// @pre To be called from game-thread.
  template <typename TSensor>
  static void SendPixelsInRenderThread(TSensor &Sensor);

  template <typename TSensor>
  static void SendFisheyePixelsInRenderThread(TSensor &Sensor);


  template <typename TSensor>
  static void SendSplitPixelsInRenderThread(TSensor &Sensor, FIntRect &PosInRenderTarget);

  static void SendAllSplitPixelsInRenderThread(TMap<uint32, ASceneCaptureSensorMulti*> &Sensors);


  static void CopyAndSend(
	  ASceneCaptureSensorMulti *Sensor,
	  FAsyncDataStream *Stream,
	  carla::Buffer *Buffer,
	  uint32 SrcStride,
	  FRHITexture2D *Texture,
	  const uint8 *Source);

private:

  /// Copy the pixels in @a RenderTarget into @a Buffer.
  ///
  /// @pre To be called from render-thread.
	static void WritePixelsToBuffer(
	  UTextureRenderTarget2D &RenderTarget,
	  carla::Buffer &Buffer,
	  uint32 Offset,
	  ASensor &Sensor,
      FRHICommandListImmediate &InRHICmdList);

    static void WriteFisheyePixelsToBuffer(
        UTextureRenderTarget2D *CaptureRenderTarget[5],
        carla::Buffer,
        carla::Buffer *Buffer[5],
        uint32 Offset,
        ASensor &Sensor,
        FRHICommandListImmediate &InRHICmdList);

  static void WriteSplitPixelsToBuffer(
	  UTextureRenderTarget2D &RenderTarget,
	  FIntRect &PosInRenderTarget,
	  carla::Buffer &Buffer,
	  uint32 Offset,
	  ASensor &Sensor,
	  FRHICommandListImmediate &InRHICmdList);

  static void WriteAllSplitPixelsToBuffer(
	  TMap<uint32, ASceneCaptureSensorMulti*> &Sensors, 
	  TMap<uint32, FAsyncDataStream*> Streams, 
	  TMap<uint32, carla::Buffer*> Buffers,
	  FRHICommandListImmediate &InRHICmdList);

};

// =============================================================================
// -- FPixelReader::SendPixelsInRenderThread -----------------------------------
// =============================================================================

template <typename TSensor>
void FPixelReader::SendFisheyePixelsInRenderThread(TSensor &Sensor)
{
    for(int i=0; i<5; i++)
    {
        check(Sensor.CaptureRenderTarget[i] != nullptr);
    }
    // Enqueue a command in the render-thread that will write the image buffer to
    // the data stream. The stream is created in the capture thus executed in the
    // game-thread.

    ENQUEUE_RENDER_COMMAND(FWriteFisheyePixelsToBuffer_SendPixelsInRenderThread)
        (
            [&Sensor, Stream = Sensor.GetDataStream(Sensor)](auto &InRHICmdList) mutable
    {
        if (!Sensor.IsPendingKill())
        {

            auto t1 = std::chrono::system_clock::now();

            auto Buffer = Stream.PopBufferFromPool();
            carla::Buffer *Buffers[5];
            for (int i = 0; i < 5; i++)
            {
                Buffers[i] = Stream.PopBufferFromPool();
            }

            auto t2 = std::chrono::system_clock::now();

            WriteFisheyePixelsToBuffer(
                Sensor.CaptureRenderTarget,
                Buffer,
                Buffers,
                carla::sensor::SensorRegistry::get<TSensor *>::type::header_offset,
                Sensor,
                InRHICmdList);

            auto t3 = std::chrono::system_clock::now();

            Stream.Send(Sensor, std::move(Buffer));

            auto t4 = std::chrono::system_clock::now();
            std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;
            std::chrono::duration<double> elapsed_seconds_2 = t3 - t2;
            std::chrono::duration<double> elapsed_seconds_3 = t4 - t3;
            //WritePixelsToBuffer��ʱ5ms
            UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time SendPixelsInRenderThread  , %.8lf, %.8lf, %.8lf"), elapsed_seconds_1.count(), elapsed_seconds_2.count(), elapsed_seconds_3.count());
        }


    }
    );
}


template <typename TSensor>
void FPixelReader::SendPixelsInRenderThread(TSensor &Sensor)
{
  check(Sensor.CaptureRenderTarget != nullptr);
  // Enqueue a command in the render-thread that will write the image buffer to
  // the data stream. The stream is created in the capture thus executed in the
  // game-thread.
  
  ENQUEUE_RENDER_COMMAND(FWritePixels_SendPixelsInRenderThread)
  (
	  [&Sensor, Stream=Sensor.GetDataStream(Sensor)](auto &InRHICmdList) mutable
		{
	      /// @todo Can we make sure the sensor is not going to be destroyed?
	      if (!Sensor.IsPendingKill())
	      {

	      	auto t1 = std::chrono::system_clock::now();

	      	auto Buffer = Stream.PopBufferFromPool();

			auto t2 = std::chrono::system_clock::now();

	        WritePixelsToBuffer(
	            *Sensor.CaptureRenderTarget,
	            Buffer,
	            carla::sensor::SensorRegistry::get<TSensor *>::type::header_offset,
				Sensor,
	            InRHICmdList);

			auto t3 = std::chrono::system_clock::now();

	        Stream.Send(Sensor, std::move(Buffer));

			auto t4 = std::chrono::system_clock::now();
			std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;
			std::chrono::duration<double> elapsed_seconds_2 = t3 - t2;
			std::chrono::duration<double> elapsed_seconds_3 = t4 - t3;
			//WritePixelsToBuffer��ʱ5ms
			UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time SendPixelsInRenderThread  , %.8lf, %.8lf, %.8lf"), elapsed_seconds_1.count(), elapsed_seconds_2.count(), elapsed_seconds_3.count());
	      }

		}
  );
}

//template <typename TSensor>
//void FPixelReader::SendPixelsInRenderThread(TSensor &Sensor)
//{
//	check(Sensor.CaptureRenderTarget != nullptr);
//	// Enqueue a command in the render-thread that will write the image buffer to
//	// the data stream. The stream is created in the capture thus executed in the
//	// game-thread.
//	ENQUEUE_RENDER_COMMAND(FWritePixels_SendPixelsInRenderThread)
//		(
//			[&Sensor](auto &InRHICmdList) mutable
//	{
//		/// @todo Can we make sure the sensor is not going to be destroyed?
//		if (!Sensor.IsPendingKill())
//		{
//			auto Stream = Sensor.GetDataStreamPtr(Sensor);
//			auto Buffer = new carla::Buffer;
//			WritePixelsToBuffer(
//				*Sensor.CaptureRenderTarget,
//				*Buffer,
//				carla::sensor::SensorRegistry::get<TSensor *>::type::header_offset,
//				Sensor,
//				InRHICmdList);
//			Stream->Send(Sensor, std::move(*Buffer));
//			delete Stream;
//			delete Buffer;
//		}
//	}
//	);
//}



// =============================================================================
// -- FPixelReader::SendSplitPixelsInRenderThread -----------------------------------
// =============================================================================

template <typename TSensor>
void FPixelReader::SendSplitPixelsInRenderThread(TSensor &Sensor, FIntRect &PosInRenderTarget)
{
	check(Sensor.CaptureRenderTarget != nullptr);

	// Enqueue a command in the render-thread that will write the image buffer to
	// the data stream. The stream is created in the capture thus executed in the
	// game-thread.
	ENQUEUE_RENDER_COMMAND(FWriteSplitPixels_SendSplitPixelsInRenderThread)
	(
		[&Sensor, &PosInRenderTarget, Stream = Sensor.GetDataStream(Sensor)](auto &InRHICmdList) mutable
		{
			/// @todo Can we make sure the sensor is not going to be destroyed?
			if (!Sensor.IsPendingKill() && Sensor.CaptureComponent2DMulti->CameraAttributeMap.Contains(Sensor.ID))
			{
				auto t1 = std::chrono::system_clock::now();

				auto Buffer = Stream.PopBufferFromPool();

				auto t2 = std::chrono::system_clock::now();

				WriteSplitPixelsToBuffer(
					*Sensor.CaptureRenderTarget,
					PosInRenderTarget,
					Buffer,
					carla::sensor::SensorRegistry::get<TSensor *>::type::header_offset,
					Sensor,
					InRHICmdList);

				auto t3 = std::chrono::system_clock::now();

				Stream.Send(Sensor, std::move(Buffer));

				auto t4 = std::chrono::system_clock::now();

				std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;
				std::chrono::duration<double> elapsed_seconds_2 = t3 - t2;
				std::chrono::duration<double> elapsed_seconds_3 = t4 - t3;

				UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time SendSplitPixelsInRenderThread , %.8lf, %.8lf, %.8lf"), elapsed_seconds_1.count(), elapsed_seconds_2.count(), elapsed_seconds_3.count());;

			}
		}
	);
}