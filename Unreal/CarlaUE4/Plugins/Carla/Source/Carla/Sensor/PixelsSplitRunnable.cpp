 // Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma de Barcelona (UAB). This work is licensed under the terms of the MIT license. For a copy, see <https://opensource.org/licenses/MIT>.


 #include "PixelsSplitRunnable.h"

 #include <carla/Buffer.h>
 #include "Runnable.h"
 #include "chrono"
#include "ContentStreaming.h"
#include "GenericPlatformAffinity.h"
 #include "PixelReader.h"
 #include "WindowsPlatformProcess.h"
 #include "HAL/RunnableThread.h"
 #include "Carla/Sensor/PixelReader.h"
#include  "Carla/Sensor/SceneCaptureSensorMulti.h"


//初始化静态成员变量:ThreadMap
TMap<FString, FPixelsSplitRunnable*>FPixelsSplitRunnable::ThreadMap = TMap<FString, FPixelsSplitRunnable*>();

//构造函数用于初始化创建线程实例
FPixelsSplitRunnable::FPixelsSplitRunnable(const FString& Name) : ThreadName(Name)
{
	//获取FEvent指针
	ThreadEvent = FPlatformProcess::GetSynchEventFromPool();


	/*
	*	InRunnable:需要传入一个FRunnable派生类的对象,这里直接传入this
	*	ThreadName:线程名
	*	InStackSize:要创建的堆栈的大小,0表示使用当前线程的堆栈大小.默认传入0即可
	*	InThreadPri:告诉线程是否需要调整其优先级。 默认为正常优先级,在这里使用低于正常优先级
	*/
	ThreadIns = FRunnableThread::Create(this, *Name, 0, TPri_BelowNormal);
}

//析构函数用于回收内存
FPixelsSplitRunnable::~FPixelsSplitRunnable()
{
	/*析构函数中手动回收内存*/
	delete ThreadIns;
	ThreadIns = nullptr;
	delete ThreadEvent;
	ThreadEvent = nullptr;
}

void FPixelsSplitRunnable::CopyAndSend()
{
	//IStreamingManager::Get().AddViewInformation(
	//Sensor->GetActorLocation(),
	//Sensor->GetImageWidth(),
	//Sensor->GetImageHeight() / FMath::Tan(Sensor->GetFOVAngle()));
	FIntRect PosInRenderTarget = Sensor->GetPosInRendertarget();
	//UE_LOG(LogTemp, Warning, TEXT("Jarvan Texture width %d height %d"), Texture->GetSizeX(), Texture->GetSizeY());
	//UE_LOG(LogTemp, Warning, TEXT("Jarvan Pos min x %d y %d , max x %d y %d "),
	//	PosInRenderTarget.Min.X, PosInRenderTarget.Min.Y, PosInRenderTarget.Max.X, PosInRenderTarget.Max.Y);
	if (Texture->GetSizeX() < static_cast<uint32>(PosInRenderTarget.Max.X) || 
		Texture->GetSizeY() < static_cast<uint32>(PosInRenderTarget.Max.Y))
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
	UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time sensor in CopyAndSend No.%d copy , %.8lf, %.8lf"), Sensor->ID, elapsed_seconds_1.count(), elapsed_seconds_2.count());
}











//接口:Init 初始化逻辑
bool FPixelsSplitRunnable::Init()
{
	UE_LOG(LogTemp, Warning, TEXT("bool FPixelsSplitRunnable::Init() !"));
	//没有需要执行的初始化逻辑,返回true即可.ps:返回false,将线程初始化失败,后续不会执行
	return true;
}

//执行线程逻辑函数
uint32 FPixelsSplitRunnable::Run()
{
	UE_LOG(LogTemp, Warning, TEXT("uint32 FPixelsSplitRunnable::Run() !"));
	//线程中使用Sleep来延迟逻辑执行时机
	FPlatformProcess::Sleep(0.03);

	int IntVar = 0;

	/*线程中使用while来实现一个持续执行的效果
	*我们使用bool值bRun来作为一个简单的条件
	*当bRun=true时,将认为线程工作尚未完成,会继续执行
	*/
	while (bRun)
	{
		/*
		*使用bool值 bPause作为线程挂起的条件
		*bPause=true时,在Run函数中挂起线程,注意,要在本线程中挂起才可以!!!
		*/
		//UE_LOG(LogTemp, Warning, TEXT("Thread :%s is running before! IntVar:%d"), *ThreadName, IntVar);
		if (bPause)
		{
			/*
			*这里着重说明!!!
			*一个线程实例对应一个FEvent指针,只有自己的FEvent指针能把自己挂起!!
			*当时用FEvent指针控制线程挂起时,一定要在想要挂起的线程中执行Wait()
			*因为Wait函数的特性为,将执行它的线程挂起,也就是说,如果在主线程中执行Wait,住线程将会被挂起
			*这会导致主线程看上去像是卡死了!!
			*/
			ThreadEvent->Wait();
		}

		//while中书写想要持续执行的线程逻辑,比如每隔一秒打印一句话

		UE_LOG(LogTemp, Warning, TEXT("Thread :%s is running! IntVar:%d"), *ThreadName, IntVar);
		IntVar++;

		CopyAndSend();
		bPause = true;
		//这里Sleep 一秒是为了限制while的执行频率,减少资源损耗
		//FPlatformProcess::Sleep(1.f);
	}
	//while执行完成之后,Run函数返回,这代表着线程逻辑执行完成
	return 0;
}

//接口:关闭线程执行条件
void FPixelsSplitRunnable::Stop()
{
	bRun = false;
}

//接口:线程退出后自动执行
void FPixelsSplitRunnable::Exit()
{
	UE_LOG(LogTemp, Warning, TEXT("Thread:%s is Exit"), *ThreadName);
}

//FEvent指针挂起线程
void FPixelsSplitRunnable::PauseThread()
{
	/*当Run函数中检测到bPause=true,会执行 ThreadEvent->Wait(),将本线程挂起
	*注意,当线程被挂起之后,Run函数不再执行,需要在其他线程把本线程唤醒才可以
	*自己无法唤醒自己!
	*/
	bPause = true;
}

//FEvent指针唤醒线程
void FPixelsSplitRunnable::WakeUpThread()
{
	bPause = false;
	/*FEvent指针执行Trigger即可将线程唤醒.
	*要记住的是,一个线程对应一个FEvent指针,只有自己的FEvent指针能够把自己唤醒
	*/
	ThreadEvent->Trigger();
}

//确保线程工作执行完成
void FPixelsSplitRunnable::EnsureCompletion()
{
	//关闭线程执行条件
	Stop();

	//FRunnableThread实例,执行WaitForCompletion,会等待线程执行完毕才会结束线程
	ThreadIns->WaitForCompletion();
}

/*****************静态函数调用****************/

//初始化FRunnable实例
FPixelsSplitRunnable* FPixelsSplitRunnable::JoyInit(const FString& ThreadName)
{
	FPixelsSplitRunnable* PNW = nullptr;
	//判断Runnable是否存在以及是否支持多线程
	if (FPlatformProcess::SupportsMultithreading())
	{
		PNW = new FPixelsSplitRunnable(ThreadName);
		//将FRunnable实例存入ThreadMap中
		ThreadMap.Add(ThreadName, PNW);
	}
	return PNW;
}

//静态函数:挂起or唤醒线程
void FPixelsSplitRunnable::Suspend(const FString& ThreadName, bool bSuspend, bool bUseSuspend/*=true*/)
{
	if (FPixelsSplitRunnable* Runnable = *ThreadMap.Find(ThreadName))
	{
		if (bUseSuspend)
		{
			/*线程激活/挂起方式一:
			*该种方式可以直接通过FRunnableThread::Suspend(true/false)的方式挂起或者激活线程
			*/
			Runnable->ThreadIns->Suspend(bSuspend);
		}
		else
		{
			/*线程激活/挂起方式二:
			*/
			if (bSuspend)
			{
				Runnable->PauseThread();
			}
			else
			{
				Runnable->WakeUpThread();
			}
		}
	}
}

bool FPixelsSplitRunnable::IsThreadPause(const FString& ThreadName)
{
    if (ThreadMap.Find(ThreadName))
    {
        FPixelsSplitRunnable* Runnable = *ThreadMap.Find(ThreadName);
        if(Runnable->bRun)
        {
            return Runnable->bPause;
        }
    }
    return true;
}

//静态函数:判断线程是否完成
bool FPixelsSplitRunnable::IsThreadFinished(const FString& ThreadName)
{
	if (ThreadMap.Find(ThreadName))
	{
		FPixelsSplitRunnable* Runnable = *ThreadMap.Find(ThreadName);
		return Runnable->bRun;
	}
	return true;
}

//静态函数:结束线程
void FPixelsSplitRunnable::Shutdown(const FString& ThreadName)
{
	if (ThreadMap.Find(ThreadName))
	{
		FPixelsSplitRunnable* Runnable = *ThreadMap.Find(ThreadName);
		Runnable->EnsureCompletion();
		ThreadMap.Remove(ThreadName);
	}
}

//静态函数:获取静态线程存储容器 ThreadMap
TMap<FString, FPixelsSplitRunnable*> FPixelsSplitRunnable::GetThreadMap()
{
	return ThreadMap;
}



 //FPixelsSplitRunnable::FPixelsSplitRunnable(const FString& ThreadNamePara)
 //{
 //	bRunning = true;
 //	bPause = false;
 //	// 创建线程实例
 //	ThreadName = ThreadNamePara;
 //	ThreadImpl = FRunnableThread::Create(this, *ThreadName, 0, EThreadPriority::TPri_Highest);
 //	ThreadID = ThreadImpl->GetThreadID();
	//UE_LOG(LogTemp, Warning, TEXT("Thread Start! ThreadID = %d"), ThreadID);
 //}
 //FPixelsSplitRunnable::~FPixelsSplitRunnable()
 //{
 //	if (ThreadImpl)		// 清空 FRunnableThread*
 //	{
 //		delete ThreadImpl;
 //		ThreadImpl = nullptr;
 //	}
 //	UE_LOG(LogTemp, Warning, TEXT("~FPixelsSplitRunnable! ThreadID = %d"), ThreadID);
 //}

 //bool FPixelsSplitRunnable::Init()
 //{
	//if(ThreadImpl)
	//{
	//	ThreadID = ThreadImpl->GetThreadID();
	//	UE_LOG(LogTemp, Warning, TEXT("Thread Init! ThreadID = %d"), ThreadID);
	//}else
	//{
	//	UE_LOG(LogTemp, Warning, TEXT("Thread Init! ThreadImpl gone !"));
	//}
 //	/*UE_LOG(LogTemp, Warning, TEXT("Thread Init! ThreadID = %d"), ThreadID);*/
 //	//PauseThread();
 //	return true; //若返回 false, 线程创建失败，不会执行后续函数
 //}

 //uint32 FPixelsSplitRunnable::Run()
 //{
	// UE_LOG(LogTemp, Warning, TEXT("Thread in run! start !"));
	// if (ThreadImpl)
	// {
	//	 ThreadID = ThreadImpl->GetThreadID();
	//	 UE_LOG(LogTemp, Warning, TEXT("Thread run! ThreadID = %d"), ThreadID);
	// }
	// else
	// {
	//	 UE_LOG(LogTemp, Warning, TEXT("Thread run! ThreadImpl gone !"));
	// }
 //	//UE_LOG(LogTemp, Warning, TEXT("Thread in Run! ThreadID = %d"), ThreadID);

 //	auto t5 = std::chrono::system_clock::now();

 //	//while (bRunning)
 //	//{
	//	//count++;
	//	//if(count %  == 0)
	//	//{
	//	//	if (bPause)
	//	//	{
	//	//		UE_LOG(LogTemp, Warning, TEXT("Thread in Run while and pause! Escape ! ThreadID = %d"), ThreadID);
	//	//	}
	//	//	else
	//	//	{
	//	//		UE_LOG(LogTemp, Warning, TEXT("Thread Running! ThreadID = %d"), ThreadID);
	//	//	}
	//	//	
	//	//}
 //	//	//FPixelReader::CopyAndSend(Sensor, Stream, Buffer, SrcStride, Texture, Source);
 //	//	//PauseThread();
 //	FPlatformProcess::Sleep(10.0f); // 执行间隔，防止堵塞
 //	//}
 //	UE_LOG(LogTemp, Warning, TEXT("Thread out Run! ThreadID = %d"), ThreadID);
 //	return 0;
 //}

 //void FPixelsSplitRunnable::Stop()
 //{
 //	bRunning = false;
 //	UE_LOG(LogTemp, Warning, TEXT("Thread Stop! ThreadID = %d"), ThreadID);
 //}

 //void FPixelsSplitRunnable::Exit()
 //{
	// if (ThreadImpl)
	// {
	//	 ThreadID = ThreadImpl->GetThreadID();
	//	 UE_LOG(LogTemp, Warning, TEXT("Thread exit! ThreadID = %d"), ThreadID);
	// }
	// else
	// {
	//	 UE_LOG(LogTemp, Warning, TEXT("Thread exit! ThreadImpl gone !"));
	// }
 //	UE_LOG(LogTemp, Warning, TEXT("Thread Exit! ThreadID = %d"), ThreadID);
 //}

 //void FPixelsSplitRunnable::PauseThread()
 //{
 //	bPause = true;
 //	Suspend(true);
 //	UE_LOG(LogTemp, Warning, TEXT("Thread PauseThread! ThreadID = %d"), ThreadID);
 //}

 //void FPixelsSplitRunnable::WakeUpThread()
 //{
 //	bPause = false;
 //	Suspend(false);
	////Run();
 //	UE_LOG(LogTemp, Warning, TEXT("Thread WakeUpThread! ThreadID = %d"), ThreadID);
 //}

 //void FPixelsSplitRunnable::StopThread()
 //{
 //	bRunning = false;
 //	bPause = false;
 //	if (ThreadImpl)
 //	{
 //		ThreadImpl->Kill(true); // bShouldWait 为false，Suspend(true)时，会崩
 //	}
 //}

 //void FPixelsSplitRunnable::Suspend(bool bSuspend)
 //{
 //	// true挂起/false唤醒
 //	if (ThreadImpl)
 //	{
 //		ThreadImpl->Suspend(bSuspend);
 //	}
 //}