// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "FisheyeCS4Camera.h"
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FFisheyeCS4CameraModule"

void FFisheyeCS4CameraModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
    FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("FisheyeCS4Camera"))->GetBaseDir(), TEXT("Shaders"));
    UE_LOG(LogTemp, Warning, TEXT("PluginShaderDir : %s"), *PluginShaderDir);
    AddShaderSourceDirectoryMapping(TEXT("/Plugin/FisheyeCS4Camera"), PluginShaderDir);
}

void FFisheyeCS4CameraModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FFisheyeCS4CameraModule, FisheyeCS4Camera)