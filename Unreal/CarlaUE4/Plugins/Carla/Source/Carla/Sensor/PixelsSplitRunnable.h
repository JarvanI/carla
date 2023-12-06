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

	 //真正的线程实例,可以通过该实例对线程进行挂起,激活,关闭等操作
	 FRunnableThread* ThreadIns;

	 //线程名称,用于创建线程实例时为线程实例命名
	 FString ThreadName;

	 //FRunnable实例,可运行的对象,复写了Init,Run,Stop,Exit接口
	 FPixelsSplitRunnable* RunnableIns;

	 //FEvent指针,可以通过该指针进行线程挂起,激活
	 FEvent* ThreadEvent;

	 //用于控制线程挂起/激活状态
	 volatile bool bPause = true;

	 //用于控制线程结束
     volatile bool bRun = true;

 public:
	 ASceneCaptureSensorMulti *Sensor = nullptr;
	 FAsyncDataStream *Stream = nullptr;
	 carla::Buffer *Buffer = nullptr;
	 uint32 SrcStride = 0;
	 FRHITexture2D *Texture = nullptr;
	 const uint8 *Source = nullptr;


 public:

	 //构造函数
	 FPixelsSplitRunnable(const FString& ThreadName);

	 //析构函数
	 ~FPixelsSplitRunnable();

	 //接口:初始化
	 virtual bool Init()override;

	 //接口:运行
	 virtual uint32 Run()override;

	 //接口:停止运行
	 virtual void Stop()override;

	 //接口:退出线程
	 virtual void Exit()override;

	 //挂起线程,本例有两种方式挂起线程,此为使用FEvent指针的方式
	 void PauseThread();

	 //激活线程,本例有两种方式唤醒线程,此为使用FEvent指针的方式
	 void WakeUpThread();

	 //确保线程执行完毕
	 void EnsureCompletion();

	 //静态调用: 实例化FTestThread对象
	 static FPixelsSplitRunnable* JoyInit(const FString& ThreadName);

	 /*静态调用: 线程挂起or唤醒
	 *ThreadName	:想要挂起or唤醒的线程名
	 *bSuspend	:执行挂起or唤醒操作
	 *bUseSuspend	:是否使用Suspend来挂起or唤醒线程(因为本例有两种方式来挂起or唤醒线程)
	 */
	 static void Suspend(const FString& ThreadName, bool bSuspend, bool bUseSuspend = true);

     //用于判断某个线程是否暂停
     static bool IsThreadPause(const FString& ThreadName);

	 //静态调用: 判断一个线程是否结束
	 static bool IsThreadFinished(const FString& ThreadName);

	 //静态调用:结束一个线程的运行
	 static void Shutdown(const FString& ThreadName);

	 //静态调用:获取 ThreadName:ThreadIns Map容器,该静态容器用于存储创建的多个线程实例
	 static TMap<FString, FPixelsSplitRunnable*> GetThreadMap();


	 void CopyAndSend();

 private:

	 //线程容器
	 static TMap<FString, FPixelsSplitRunnable*>ThreadMap;
 };


 //class CARLA_API FPixelsSplitRunnable : public FRunnable
 //{
	// friend class FPixelReader;
 //public:
	// FPixelsSplitRunnable(const FString& ThreadNamePara);
	// ~FPixelsSplitRunnable();

	// void PauseThread();				// 线程挂起

	// void WakeUpThread();			// 线程唤醒

	// void StopThread();				// 停止线程


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

	// bool bRunning;				// 线程循环标志

	// bool bPause;			// 线程挂起标志

	// FRunnableThread* ThreadImpl;		// 线程实例

	// int count = 0;

	// //override Init,Run and Stop.
	// virtual bool Init() override; // 初始化 runnable 对象，在FRunnableThread创建线程对象后调用

	// virtual uint32 Run() override; // Runnable 对象逻辑处理主体，在Init成功后调用

	// virtual void Stop() override; // 停止 runnable 对象, 线程提前终止时被用户调用

	// virtual void Exit() override; // 退出 runnable 对象，由FRunnableThread调用

	// void Suspend(bool bSuspend);	// 线程挂起/唤醒 , 被WakeUpThread和StopThread调用
 //};