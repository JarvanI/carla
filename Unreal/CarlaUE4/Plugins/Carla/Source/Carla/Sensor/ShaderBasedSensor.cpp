// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "Carla.h"
#include "Carla/Sensor/ShaderBasedSensor.h"

#include "ConstructorHelpers.h"
#include "Materials/MaterialInstanceDynamic.h"

//从路径读取material uasset文件 , 以UMaterial指针的形式存储在数组MaterialsFound中
bool AShaderBasedSensor::AddPostProcessingMaterial(const FString &Path)
{
  ConstructorHelpers::FObjectFinder<UMaterial> Loader(*Path);
  if (Loader.Succeeded())
  {
    MaterialsFound.Add(Loader.Object);
  }
  return Loader.Succeeded();
}

void AShaderBasedSensor::SetUpSceneCaptureComponent(USceneCaptureComponent2D &SceneCapture)
{
  // 依次遍历数组MaterialsFound中所有的UMaterial , 根据UMaterial生成UMaterialInstanceDynamic , 并组装成FSensorShader存储在数组Shaders中
  for (const auto &MaterialFound : MaterialsFound)
  {
    // Create a dynamic instance of the Material (Shader)
    AddShader({UMaterialInstanceDynamic::Create(MaterialFound, this), 1.0});
  }

  // djw tbd : 看PostProcessSettings.AddBlendable函数
  // 看了下CalcSceneView , ViewFamily中的view也有各自的PostProcessSetting . 另外 , shaderbasedsensor里也都是对图像做后处理 , 
  // 不同类型的camera渲染方式其实是一样的 , 只是后处理不同 . 所以2dmulti中也可以一帧出来的总图像 , 也可以通过对每个view设置不同的PostProcessSettings , 来实习不同的效果 ?
  for (const auto &Shader : Shaders)
  {
    // Attach the instance into the blendables
    SceneCapture.PostProcessSettings.AddBlendable(Shader.PostProcessMaterial, Shader.Weight);
  }

  // 读取FloatShaderParams数组的值, 修改Shader数组中对应下标元素中的UMaterialInstanceDynamicl类型元素
  // Set the value for each Float parameter in the shader
  for (const auto &ParameterValue : FloatShaderParams)
  {
    Shaders[ParameterValue.ShaderIndex].PostProcessMaterial->SetScalarParameterValue(
        ParameterValue.ParameterName,
        ParameterValue.Value);
  }
}

void AShaderBasedSensor::Set(const FActorDescription &Description)
{
  //先执行ASensor的Set方法 , 然后执行ASceneCaptureSensor的Set方法
  Super::Set(Description);
  UActorBlueprintFunctionLibrary::SetCamera(Description, this);
}

// 搜索了下 , 只有在void UActorBlueprintFunctionLibrary::SetCamera(const FActorDescription &Description,AShaderBasedSensor *Camera)
// 函数中才有调用
void AShaderBasedSensor::SetFloatShaderParameter(
    uint8_t ShaderIndex,
    const FName &ParameterName,
    float Value)
{
  FloatShaderParams.Add({ShaderIndex, ParameterName, Value});
}
