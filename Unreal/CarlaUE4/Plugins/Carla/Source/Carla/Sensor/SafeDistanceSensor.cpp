// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma de Barcelona (UAB). This work is licensed under the terms of the MIT license. For a copy, see <https://opensource.org/licenses/MIT>.


#include "Carla.h"
#include "Carla/Sensor/SafeDistanceSensor.h"

#include "Carla/Actor/ActorBlueprintFunctionLibrary.h"
#include "Carla/Game/CarlaEpisode.h"
#include "Carla/Util/BoundingBoxCalculator.h"
#include "Carla/Vehicle/CarlaWheeledVehicle.h"


ASafeDistanceSensor::ASafeDistanceSensor(const FObjectInitializer &ObjectInitializer)
	: Super(ObjectInitializer)
{
	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("BoxOverlap"));
	Box->SetupAttachment(RootComponent);
	Box->SetHiddenInGame(true); // Disable for debugging.
	Box->SetCollisionProfileName(FName("OverlapAll"));

	PrimaryActorTick.bCanEverTick = true;
}

FActorDefinition ASafeDistanceSensor::GetSensorDefinition()
{
	// 使得python中调用下一句的时候可以找到该传感器
	// bp = world.get_blueprint_library().find('sensor.other.safe_distance')
	auto Definition = UActorBlueprintFunctionLibrary::MakeGenericSensorDefinition(
		TEXT("other"),
		TEXT("safe_distance"));

	FActorVariation Front;
	Front.Id = TEXT("safe_distance_front");
	Front.Type = EActorAttributeType::Float;
	Front.RecommendedValues = { TEXT("1.0") };
	Front.bRestrictToRecommended = false;

	FActorVariation Back;
	Back.Id = TEXT("safe_distance_back");
	Back.Type = EActorAttributeType::Float;
	Back.RecommendedValues = { TEXT("0.5") };
	Back.bRestrictToRecommended = false;

	FActorVariation Lateral;
	Lateral.Id = TEXT("safe_distance_lateral");
	Lateral.Type = EActorAttributeType::Float;
	Lateral.RecommendedValues = { TEXT("0.5") };
	Lateral.bRestrictToRecommended = false;

	Definition.Variations.Append({ Front, Back, Lateral });

	return Definition;
}


void ASafeDistanceSensor::Set(const FActorDescription &Description)
{
	Super::Set(Description);

	float Front = UActorBlueprintFunctionLibrary::RetrieveActorAttributeToFloat(
		"safe_distance_front",
		Description.Variations,
		1.0f);
	float Back = UActorBlueprintFunctionLibrary::RetrieveActorAttributeToFloat(
		"safe_distance_back",
		Description.Variations,
		0.5f);
	float Lateral = UActorBlueprintFunctionLibrary::RetrieveActorAttributeToFloat(
		"safe_distance_lateral",
		Description.Variations,
		0.5f);

	constexpr float M_TO_CM = 100.0f; // Unit conversion.

	float LocationX = M_TO_CM * (Front - Back) / 2.0f;
	float ExtentX = M_TO_CM * (Front + Back) / 2.0f;
	float ExtentY = M_TO_CM * Lateral;

	Box->SetRelativeLocation(FVector{ LocationX, 0.0f, 0.0f });
	Box->SetBoxExtent(FVector{ ExtentX, ExtentY, 0.0f });
}


// SetOwner函数继承自AActor类 , 在调用SpawnActor函数后 , 在PostSpawnInitialize函数中调用SetOwner函数
// SpawnActor函数的分析见https://zhuanlan.zhihu.com/p/499116106
// PostSpawnInitialize函数见https://zhuanlan.zhihu.com/p/499565681
void ASafeDistanceSensor::SetOwner(AActor *Owner)
{
	Super::SetOwner(Owner);

	auto BoundingBox = UBoundingBoxCalculator::GetActorBoundingBox(Owner);

	Box->SetBoxExtent(BoundingBox.Extent + Box->GetUnscaledBoxExtent());
}


void ASafeDistanceSensor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	TSet<AActor *> DetectedActors;
	Box->GetOverlappingActors(DetectedActors, ACarlaWheeledVehicle::StaticClass());
	DetectedActors.Remove(GetOwner());

	if (DetectedActors.Num() > 0)
	{
		auto Stream = GetDataStream(*this);
		// 这里Send中要发送sensor的this指针 , 也是因为注册sensor的时候要往map类型的SensorRegistry中插入sensor类型为key值, 
		// 对应的sensor data serializer为value . 这样程序就知道要调用哪个serializer来处理数据了
		Stream.Send(*this, GetEpisode(), DetectedActors);
	}
}