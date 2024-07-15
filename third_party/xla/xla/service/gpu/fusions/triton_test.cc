/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "xla/service/gpu/fusions/triton.h"

#include <memory>
#include <optional>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/service/gpu/fusions/fusion_emitter.h"
#include "xla/service/gpu/fusions/fusions.h"
#include "xla/service/gpu/gpu_device_info_for_tests.h"
#include "xla/service/gpu/hlo_fusion_analysis.h"
#include "xla/stream_executor/device_description.h"
#include "xla/tests/hlo_test_base.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace gpu {
namespace {

using ::testing::ElementsAre;

class TritonFusionTest : public HloTestBase {};

TEST_F(TritonFusionTest,
       TritonFusionWithBlockLevelFusionConfig_LaunchConfigIsCorrect) {
  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(R"(
triton_computation {
  param_0 = f32[125,127] parameter(0)
  ROOT abs = f32[125,127] abs(param_0)
}

ENTRY entry_computation {
  param_0 = f32[125,127] parameter(0)
  ROOT fusion.1 = f32[125,127] fusion(param_0), kind=kCustom,
    calls=triton_computation,
    backend_config={"fusion_backend_config":{
      "kind":"__triton",
      "block_level_fusion_config":{"output_tile_sizes":["3","127"],
                                   "num_warps":"4"}}}
})"));

  stream_executor::DeviceDescription device_info =
      TestGpuDeviceInfo::RTXA6000DeviceInfo();

  auto* root = module->entry_computation()->root_instruction();
  HloFusionAnalysis analysis = AnalyzeFusion(*root, device_info);

  std::unique_ptr<FusionInterface> emitter =
      GetFusionEmitter(PreBufferAssignmentFusionInfo{analysis});
  auto triton_fusion = dynamic_cast<TritonFusion*>(emitter.get());
  ASSERT_NE(triton_fusion, nullptr);
  std::optional<TritonFusion::LaunchConfig> launch_config =
      triton_fusion->launch_config();
  ASSERT_NE(launch_config, std::nullopt);
  EXPECT_EQ(launch_config->launch_dimensions.num_blocks(),
            /*ceil(125 / 3)=*/42);
  EXPECT_EQ(launch_config->launch_dimensions.num_threads_per_block(),
            /*32 * num_warps=*/128);
  EXPECT_THAT(launch_config->block_level_parameters.output_tile_sizes,
              ElementsAre(3, 127));
}

TEST_F(TritonFusionTest,
       TritonFusionWithoutBlockLevelFusionConfig_LaunchConfigIsNullopt) {
  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(R"(
triton_computation {
  param_0 = f32[125,127] parameter(0)
  ROOT abs = f32[125,127] abs(param_0)
}

ENTRY entry_computation {
  param_0 = f32[125,127] parameter(0)
  ROOT fusion = f32[125,127] fusion(param_0), kind=kCustom,
    calls=triton_computation,
    backend_config={"fusion_backend_config":{"kind":"__triton"}}
})"));

  stream_executor::DeviceDescription device_info =
      TestGpuDeviceInfo::RTXA6000DeviceInfo();

  auto* root = module->entry_computation()->root_instruction();
  HloFusionAnalysis analysis = AnalyzeFusion(*root, device_info);

  std::unique_ptr<FusionInterface> emitter =
      GetFusionEmitter(PreBufferAssignmentFusionInfo{analysis});
  auto triton_fusion = dynamic_cast<TritonFusion*>(emitter.get());
  ASSERT_NE(triton_fusion, nullptr);
  EXPECT_EQ(triton_fusion->launch_config(), std::nullopt);
}

}  // namespace
}  // namespace gpu
}  // namespace xla
