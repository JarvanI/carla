 // Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma de Barcelona (UAB). This work is licensed under the terms of the MIT license. For a copy, see <https://opensource.org/licenses/MIT>.

 #pragma once

 #include "CoreMinimal.h"
 #include "Runnable.h"
#include <carla/Buffer.h>
#include "AsyncDataStream.h"
 /**
  * 
  */


 class ASceneCaptureSensorMulti;
 class FRHITexture2D;




 class CARLA_API FPixelsSplitRunnable :public FRunnable
 {
 public:

	 //�������߳�ʵ��,����ͨ����ʵ�����߳̽��й���,����,�رյȲ���
	 FRunnableThread* ThreadIns;

	 //�߳�����,���ڴ����߳�ʵ��ʱΪ�߳�ʵ������
	 FString ThreadName;

	 //FRunnableʵ��,�����еĶ���,��д��Init,Run,Stop,Exit�ӿ�
	 FPixelsSplitRunnable* RunnableIns;

	 //FEventָ��,����ͨ����ָ������̹߳���,����
	 FEvent* ThreadEvent;

	 //���ڿ����̹߳���/����״̬
	 volatile bool bPause = true;

	 //���ڿ����߳̽���
     volatile bool bRun = true;

 public:
	 ASceneCaptureSensorMulti *Sensor = nullptr;
	 FAsyncDataStream *Stream = nullptr;
	 carla::Buffer *Buffer = nullptr;
	 uint32 SrcStride = 0;
	 FRHITexture2D *Texture = nullptr;
	 const uint8 *Source = nullptr;


 public:

	 //���캯��
	 FPixelsSplitRunnable(const FString& ThreadName);

	 //��������
	 ~FPixelsSplitRunnable();

	 //�ӿ�:��ʼ��
	 virtual bool Init()override;

	 //�ӿ�:����
	 virtual uint32 Run()override;

	 //�ӿ�:ֹͣ����
	 virtual void Stop()override;

	 //�ӿ�:�˳��߳�
	 virtual void Exit()override;

	 //�����߳�,���������ַ�ʽ�����߳�,��Ϊʹ��FEventָ��ķ�ʽ
	 void PauseThread();

	 //�����߳�,���������ַ�ʽ�����߳�,��Ϊʹ��FEventָ��ķ�ʽ
	 void WakeUpThread();

	 //ȷ���߳�ִ�����
	 void EnsureCompletion();

	 //��̬����: ʵ����FTestThread����
	 static FPixelsSplitRunnable* JoyInit(const FString& ThreadName);

	 /*��̬����: �̹߳���or����
	 *ThreadName	:��Ҫ����or���ѵ��߳���
	 *bSuspend	:ִ�й���or���Ѳ���
	 *bUseSuspend	:�Ƿ�ʹ��Suspend������or�����߳�(��Ϊ���������ַ�ʽ������or�����߳�)
	 */
	 static void Suspend(const FString& ThreadName, bool bSuspend, bool bUseSuspend = true);

     //�����ж�ĳ���߳��Ƿ���ͣ
     static bool IsThreadPause(const FString& ThreadName);

	 //��̬����: �ж�һ���߳��Ƿ����
	 static bool IsThreadFinished(const FString& ThreadName);

	 //��̬����:����һ���̵߳�����
	 static void Shutdown(const FString& ThreadName);

	 //��̬����:��ȡ ThreadName:ThreadIns Map����,�þ�̬�������ڴ洢�����Ķ���߳�ʵ��
	 static TMap<FString, FPixelsSplitRunnable*> GetThreadMap();


	 void CopyAndSend();

 private:

	 //�߳�����
	 static TMap<FString, FPixelsSplitRunnable*>ThreadMap;
 };


 //class CARLA_API FPixelsSplitRunnable : public FRunnable
 //{
	// friend class FPixelReader;
 //public:
	// FPixelsSplitRunnable(const FString& ThreadNamePara);
	// ~FPixelsSplitRunnable();

	// void PauseThread();				// �̹߳���

	// void WakeUpThread();			// �̻߳���

	// void StopThread();				// ֹͣ�߳�


 //public:

	// //ASceneCaptureSensorMulti *Sensor = nullptr;
	// //FAsyncDataStream *Stream = nullptr;
	// //carla::Buffer *Buffer = nullptr;
	// //uint32 SrcStride = 0;
	// //FRHITexture2D *Texture = nullptr;
	// //uint8 *Source = nullptr;

 //private:

	// FString ThreadName;

	// int32 ThreadID;

	// bool bRunning;				// �߳�ѭ����־

	// bool bPause;			// �̹߳����־

	// FRunnableThread* ThreadImpl;		// �߳�ʵ��

	// int count = 0;

	// //override Init,Run and Stop.
	// virtual bool Init() override; // ��ʼ�� runnable ������FRunnableThread�����̶߳�������

	// virtual uint32 Run() override; // Runnable �����߼��������壬��Init�ɹ������

	// virtual void Stop() override; // ֹͣ runnable ����, �߳���ǰ��ֹʱ���û�����

	// virtual void Exit() override; // �˳� runnable ������FRunnableThread����

	// void Suspend(bool bSuspend);	// �̹߳���/���� , ��WakeUpThread��StopThread����
 //};