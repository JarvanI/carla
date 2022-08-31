// Copyright (c) 2019 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "carla/sensor/s11n/SafeDistanceSerializer.h"

#include "carla/sensor/data/SafeDistanceEvent.h"

namespace carla {
namespace sensor {
namespace s11n {

  // 反序列化 , 由于序列化后是个数组 , 直接用std::move , 反序列化后的数据类型是SafeDistanceEvent类型
  // SafeDistanceEvent的父类是Array<rpc::ActorId> , Array类型的父类是SensorData . 这个Array是类模板 , carla里专门用于存放反序列化后的SensorData
  SharedPtr<SensorData> SafeDistanceSerializer::Deserialize(RawData &&data) {
    return SharedPtr<SensorData>(new data::SafeDistanceEvent(std::move(data)));
  }

} // namespace s11n
} // namespace sensor
} // namespace carla