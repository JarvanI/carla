#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Engine/Classes/Kismet/BlueprintFunctionLibrary.h"
#include "ShadertestPluginBlueprintLibrary.generated.h"


//这个就是一个静态函数库，我们之所以能在蓝图脚本中调用这个DrawTestShaderRenderTarget函数就是因为这个是个静态函数库。
//为蓝图提供了一个直接调用的方法。
//DrawTestShaderRenderTarget就是帮我们从蓝图脚本里传了OutputRenderTarget, Ac, MyColor到我们的shader里。
UCLASS(MinimalAPI, meta = (ScriptName = "ShadertestPluginBlueprintLibrary"))
class UShadertestPluginBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_UCLASS_BODY()

        //UFUNCTION(BlueprintCallable, Category = "ShadertestPlugin", meta = (WorldContext = "WorldContextObject"))
        //static void DrawTestShaderRenderTarget(class UTextureRenderTarget2D* OutputRenderTarget, AActor* Ac, FLinearColor MyColor);
};
