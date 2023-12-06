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


//��ʼ����̬��Ա����:ThreadMap
TMap<FString, FPixelsSplitRunnable*>FPixelsSplitRunnable::ThreadMap = TMap<FString, FPixelsSplitRunnable*>();

//���캯�����ڳ�ʼ�������߳�ʵ��
FPixelsSplitRunnable::FPixelsSplitRunnable(const FString& Name) : ThreadName(Name)
{
	//��ȡFEventָ��
	ThreadEvent = FPlatformProcess::GetSynchEventFromPool();


	/*
	*	InRunnable:��Ҫ����һ��FRunnable������Ķ���,����ֱ�Ӵ���this
	*	ThreadName:�߳���
	*	InStackSize:Ҫ�����Ķ�ջ�Ĵ�С,0��ʾʹ�õ�ǰ�̵߳Ķ�ջ��С.Ĭ�ϴ���0����
	*	InThreadPri:�����߳��Ƿ���Ҫ���������ȼ��� Ĭ��Ϊ�������ȼ�,������ʹ�õ����������ȼ�
	*/
	ThreadIns = FRunnableThread::Create(this, *Name, 0, TPri_BelowNormal);
}

//�����������ڻ����ڴ�
FPixelsSplitRunnable::~FPixelsSplitRunnable()
{
	/*�����������ֶ������ڴ�*/
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











//�ӿ�:Init ��ʼ���߼�
bool FPixelsSplitRunnable::Init()
{
	UE_LOG(LogTemp, Warning, TEXT("bool FPixelsSplitRunnable::Init() !"));
	//û����Ҫִ�еĳ�ʼ���߼�,����true����.ps:����false,���̳߳�ʼ��ʧ��,��������ִ��
	return true;
}

//ִ���߳��߼�����
uint32 FPixelsSplitRunnable::Run()
{
	UE_LOG(LogTemp, Warning, TEXT("uint32 FPixelsSplitRunnable::Run() !"));
	//�߳���ʹ��Sleep���ӳ��߼�ִ��ʱ��
	FPlatformProcess::Sleep(0.03);

	int IntVar = 0;

	/*�߳���ʹ��while��ʵ��һ������ִ�е�Ч��
	*����ʹ��boolֵbRun����Ϊһ���򵥵�����
	*��bRun=trueʱ,����Ϊ�̹߳�����δ���,�����ִ��
	*/
	while (bRun)
	{
		/*
		*ʹ��boolֵ bPause��Ϊ�̹߳��������
		*bPause=trueʱ,��Run�����й����߳�,ע��,Ҫ�ڱ��߳��й���ſ���!!!
		*/
		//UE_LOG(LogTemp, Warning, TEXT("Thread :%s is running before! IntVar:%d"), *ThreadName, IntVar);
		if (bPause)
		{
			/*
			*��������˵��!!!
			*һ���߳�ʵ����Ӧһ��FEventָ��,ֻ���Լ���FEventָ���ܰ��Լ�����!!
			*��ʱ��FEventָ������̹߳���ʱ,һ��Ҫ����Ҫ������߳���ִ��Wait()
			*��ΪWait����������Ϊ,��ִ�������̹߳���,Ҳ����˵,��������߳���ִ��Wait,ס�߳̽��ᱻ����
			*��ᵼ�����߳̿���ȥ���ǿ�����!!
			*/
			ThreadEvent->Wait();
		}

		//while����д��Ҫ����ִ�е��߳��߼�,����ÿ��һ���ӡһ�仰

		UE_LOG(LogTemp, Warning, TEXT("Thread :%s is running! IntVar:%d"), *ThreadName, IntVar);
		IntVar++;

		CopyAndSend();
		bPause = true;
		//����Sleep һ����Ϊ������while��ִ��Ƶ��,������Դ���
		//FPlatformProcess::Sleep(1.f);
	}
	//whileִ�����֮��,Run��������,��������߳��߼�ִ�����
	return 0;
}

//�ӿ�:�ر��߳�ִ������
void FPixelsSplitRunnable::Stop()
{
	bRun = false;
}

//�ӿ�:�߳��˳����Զ�ִ��
void FPixelsSplitRunnable::Exit()
{
	UE_LOG(LogTemp, Warning, TEXT("Thread:%s is Exit"), *ThreadName);
}

//FEventָ������߳�
void FPixelsSplitRunnable::PauseThread()
{
	/*��Run�����м�⵽bPause=true,��ִ�� ThreadEvent->Wait(),�����̹߳���
	*ע��,���̱߳�����֮��,Run��������ִ��,��Ҫ�������̰߳ѱ��̻߳��Ѳſ���
	*�Լ��޷������Լ�!
	*/
	bPause = true;
}

//FEventָ�뻽���߳�
void FPixelsSplitRunnable::WakeUpThread()
{
	bPause = false;
	/*FEventָ��ִ��Trigger���ɽ��̻߳���.
	*Ҫ��ס����,һ���̶߳�Ӧһ��FEventָ��,ֻ���Լ���FEventָ���ܹ����Լ�����
	*/
	ThreadEvent->Trigger();
}

//ȷ���̹߳���ִ�����
void FPixelsSplitRunnable::EnsureCompletion()
{
	//�ر��߳�ִ������
	Stop();

	//FRunnableThreadʵ��,ִ��WaitForCompletion,��ȴ��߳�ִ����ϲŻ�����߳�
	ThreadIns->WaitForCompletion();
}

/*****************��̬��������****************/

//��ʼ��FRunnableʵ��
FPixelsSplitRunnable* FPixelsSplitRunnable::JoyInit(const FString& ThreadName)
{
	FPixelsSplitRunnable* PNW = nullptr;
	//�ж�Runnable�Ƿ�����Լ��Ƿ�֧�ֶ��߳�
	if (FPlatformProcess::SupportsMultithreading())
	{
		PNW = new FPixelsSplitRunnable(ThreadName);
		//��FRunnableʵ������ThreadMap��
		ThreadMap.Add(ThreadName, PNW);
	}
	return PNW;
}

//��̬����:����or�����߳�
void FPixelsSplitRunnable::Suspend(const FString& ThreadName, bool bSuspend, bool bUseSuspend/*=true*/)
{
	if (FPixelsSplitRunnable* Runnable = *ThreadMap.Find(ThreadName))
	{
		if (bUseSuspend)
		{
			/*�̼߳���/����ʽһ:
			*���ַ�ʽ����ֱ��ͨ��FRunnableThread::Suspend(true/false)�ķ�ʽ������߼����߳�
			*/
			Runnable->ThreadIns->Suspend(bSuspend);
		}
		else
		{
			/*�̼߳���/����ʽ��:
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

//��̬����:�ж��߳��Ƿ����
bool FPixelsSplitRunnable::IsThreadFinished(const FString& ThreadName)
{
	if (ThreadMap.Find(ThreadName))
	{
		FPixelsSplitRunnable* Runnable = *ThreadMap.Find(ThreadName);
		return Runnable->bRun;
	}
	return true;
}

//��̬����:�����߳�
void FPixelsSplitRunnable::Shutdown(const FString& ThreadName)
{
	if (ThreadMap.Find(ThreadName))
	{
		FPixelsSplitRunnable* Runnable = *ThreadMap.Find(ThreadName);
		Runnable->EnsureCompletion();
		ThreadMap.Remove(ThreadName);
	}
}

//��̬����:��ȡ��̬�̴߳洢���� ThreadMap
TMap<FString, FPixelsSplitRunnable*> FPixelsSplitRunnable::GetThreadMap()
{
	return ThreadMap;
}



 //FPixelsSplitRunnable::FPixelsSplitRunnable(const FString& ThreadNamePara)
 //{
 //	bRunning = true;
 //	bPause = false;
 //	// �����߳�ʵ��
 //	ThreadName = ThreadNamePara;
 //	ThreadImpl = FRunnableThread::Create(this, *ThreadName, 0, EThreadPriority::TPri_Highest);
 //	ThreadID = ThreadImpl->GetThreadID();
	//UE_LOG(LogTemp, Warning, TEXT("Thread Start! ThreadID = %d"), ThreadID);
 //}
 //FPixelsSplitRunnable::~FPixelsSplitRunnable()
 //{
 //	if (ThreadImpl)		// ��� FRunnableThread*
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
 //	return true; //������ false, �̴߳���ʧ�ܣ�����ִ�к�������
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
 //	FPlatformProcess::Sleep(10.0f); // ִ�м������ֹ����
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
 //		ThreadImpl->Kill(true); // bShouldWait Ϊfalse��Suspend(true)ʱ�����
 //	}
 //}

 //void FPixelsSplitRunnable::Suspend(bool bSuspend)
 //{
 //	// true����/false����
 //	if (ThreadImpl)
 //	{
 //		ThreadImpl->Suspend(bSuspend);
 //	}
 //}