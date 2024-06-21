// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.  

#include "NewShaderPluginBlueprintLibrary.h"


#define LOCTEXT_NAMESPACE "UShadertestPluginBlueprintLibrary"  

UNewShaderPluginBlueprintLibrary::UNewShaderPluginBlueprintLibrary(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{

}


//DrawTestShaderRenderTarget是MyShaderTest.h里蓝图函数库的实现，是逻辑线程这边调用draw方法的部分。
//void UShadertestPluginBlueprintLibrary::DrawTestShaderRenderTarget(
//    UTextureRenderTarget2D* OutputRenderTarget,
//    AActor* Ac,
//    FLinearColor MyColor
//)
//{
//    check(IsInGameThread());
//
//    if (!OutputRenderTarget)
//    {
//        return;
//    }
//
//    FTextureRenderTargetResource* TextureRenderTargetResource = OutputRenderTarget->GameThread_GetRenderTargetResource();
//    UWorld* World = Ac->GetWorld();
//
//    //获取FeatureLevel.
//    //在UE4中，FeatureLevel（特性级别）是指硬件或驱动程序所支持的图形功能级别。
//    //FeatureLevel表示了图形设备的功能水平，不同的FeatureLevel对应着不同的图形功能和性能要求。
//    //UE4支持的FeatureLevel通常包括以下几个级别：
//    //**SM5（Shader Model 5）：** 最高级别的特性级别，支持最新的硬件和图形功能，包括DX11和OpenGL 4.x级别的特性。通常用于高端PC和主机平台。
//    //**SM4（Shader Model 4）：** 较高级别的特性级别，支持DX10和OpenGL 3.x级别的特性。通常用于中高端PC和主机平台。
//    //**SM3（Shader Model 3）：** 中等级别的特性级别，支持DX9级别的特性。通常用于低端PC和移动平台。
//    //**ES2（OpenGL ES 2.0）：** 较低级别的特性级别，支持OpenGL ES 2.0级别的特性。通常用于移动设备和低端PC。
//    //不同的FeatureLevel对应着不同的图形功能和性能水平，开发人员可以根据目标平台和硬件性能选择合适的FeatureLevel来进行开发。
//    //UE4的渲染系统会根据所选的FeatureLevel来进行相应的渲染和优化，以确保在不同的硬件和平台上都能够提供良好的性能和视觉效果。
//    ERHIFeatureLevel::Type FeatureLevel = World->Scene->GetFeatureLevel();
//
//    FName TextureRenderTargetName = OutputRenderTarget->GetFName();
//    ENQUEUE_RENDER_COMMAND(CaptureCommand)(
//        [TextureRenderTargetResource, FeatureLevel, MyColor, TextureRenderTargetName](FRHICommandListImmediate& RHICmdList)
//    {
//        DrawTestShaderRenderTarget_RenderThread(RHICmdList, TextureRenderTargetResource, FeatureLevel, TextureRenderTargetName, MyColor);
//    }
//    );
//
//}

#undef LOCTEXT_NAMESPACE  