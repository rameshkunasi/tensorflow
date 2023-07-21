/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

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
#if defined(INTEL_MKL) && defined(ENABLE_ONEDNN_V3)

#include "tensorflow/compiler/xla/service/cpu/onednn_matmul.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

#include "absl/base/dynamic_annotations.h"
#include "dnnl.hpp"
#include "tensorflow/compiler/xla/executable_run_options.h"
#include "tensorflow/compiler/xla/service/cpu/backend_config.pb.h"
#include "tensorflow/compiler/xla/service/cpu/onednn_memory_util.h"
#include "tensorflow/compiler/xla/service/cpu/runtime_lightweight_check.h"
#include "tensorflow/tsl/util/onednn_threadpool.h"
#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"

namespace xla {
namespace cpu {

using namespace dnnl;

ABSL_ATTRIBUTE_NO_SANITIZE_MEMORY void __xla_cpu_runtime_OneDnnMatMul(
    const void* run_options_ptr, void* lhs, void* rhs, void* result,
    void* config) {
  const xla::ExecutableRunOptions* run_options =
      static_cast<const xla::ExecutableRunOptions*>(run_options_ptr);
  XLA_LIGHTWEIGHT_CHECK(run_options->intra_op_thread_pool() != nullptr);
  // TODO(inte-tf): Update the namespace scope of threadpool once the
  // threadpool interface wrapper is moved as tsl::OneDnnThreadPool.
  tsl::OneDnnThreadPool thread_pool(
      run_options->intra_op_thread_pool()->getPool(), false);
  engine cpu_engine(engine::kind::cpu, 0);
  auto tp_stream =
      stream(threadpool_interop::make_stream(cpu_engine, &thread_pool));

  MemrefInfo lhs_minfo(lhs);
  MemrefInfo rhs_minfo(rhs);
  MemrefInfo result_minfo(result);

  std::string config_str(static_cast<const char*>(config));
  OneDnnMatMulConfig matmul_config;
  matmul_config.ParseFromString(config_str);

  // Currently, no fusion is supported.
  XLA_LIGHTWEIGHT_CHECK(matmul_config.fused_ops().empty());

  auto src_md = lhs_minfo.GetOneDnnMemDesc();
  auto weights_md = rhs_minfo.GetOneDnnMemDesc();
  auto dst_md = result_minfo.GetOneDnnMemDesc();

  auto src_mem = memory(src_md, cpu_engine, lhs_minfo.Data());
  auto weights_mem = memory(weights_md, cpu_engine, rhs_minfo.Data());
  auto dst_mem = memory(dst_md, cpu_engine, result_minfo.Data());

  // Create primitive descriptor.
  auto matmul_pd =
      matmul::primitive_desc(cpu_engine, src_md, weights_md, dst_md);

  // Create the primitive.
  auto matmul_prim = matmul(matmul_pd);

  // Primitive arguments.
  std::unordered_map<int, memory> matmul_args;
  matmul_args.insert({DNNL_ARG_SRC, src_mem});
  matmul_args.insert({DNNL_ARG_WEIGHTS, weights_mem});
  matmul_args.insert({DNNL_ARG_DST, dst_mem});

  // Primitive execution: matrix multiplication with ReLU.
  matmul_prim.execute(tp_stream, matmul_args);
}

}  // namespace cpu
}  // namespace xla

#endif  // INTEL_MKL && ENABLE_ONEDNN_V3
