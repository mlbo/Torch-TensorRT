#include <iostream>
#include <string>
#include "core/compiler.h"
#include "gtest/gtest.h"
#include "tests/util/util.h"
#include "torch/csrc/jit/ir/irparser.h"

TEST(Converters, ATenConstantPad1dTensorConvertsCorrectly) {
  const auto graph = R"IR(
      graph(%0 : Tensor):
        %1 : int[] = prim::Constant[value=[2, 3]]()
        %2 : Scalar = prim::Constant[value=2]()
        %3 : Tensor = aten::constant_pad_nd(%0, %1, %2)
        return (%3))IR";

  auto g = std::make_shared<torch::jit::Graph>();
  torch::jit::parseIR(graph, g.get());

  auto in1 = at::randint(1, 10, {1, 3, 4}, {at::kCUDA});

  auto params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto jit_results = torch_tensorrt::tests::util::RunGraph(g, params, {in1});

  params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto trt_results = torch_tensorrt::tests::util::RunGraphEngine(g, params, {in1});

  ASSERT_TRUE(
      torch_tensorrt::tests::util::almostEqual(jit_results[0], trt_results[0].reshape_as(jit_results[0]), 2e-6));
}

TEST(Converters, ATenConstantPad1dRightZeroTensorConvertsCorrectly) {
  const auto graph = R"IR(
      graph(%0 : Tensor):
        %1 : int[] = prim::Constant[value=[2, 0]]()
        %2 : Scalar = prim::Constant[value=2]()
        %3 : Tensor = aten::constant_pad_nd(%0, %1, %2)
        return (%3))IR";

  auto g = std::make_shared<torch::jit::Graph>();
  torch::jit::parseIR(graph, g.get());

  auto in1 = at::randint(1, 10, {1, 3, 4}, {at::kCUDA});

  auto params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto jit_results = torch_tensorrt::tests::util::RunGraph(g, params, {in1});

  params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto trt_results = torch_tensorrt::tests::util::RunGraphEngine(g, params, {in1});

  ASSERT_TRUE(
      torch_tensorrt::tests::util::almostEqual(jit_results[0], trt_results[0].reshape_as(jit_results[0]), 2e-6));
}

TEST(Converters, ATenConstantPad1dTensorConvertsCorrectlyWithDynamic) {
  const auto graph = R"IR(
      graph(%0 : Tensor):
        %1 : int[] = prim::Constant[value=[2, 3]]()
        %2 : Scalar = prim::Constant[value=2]()
        %3 : Tensor = aten::constant_pad_nd(%0, %1, %2)
        return (%3))IR";

  auto g = std::make_shared<torch::jit::Graph>();
  torch::jit::parseIR(graph, g.get());

  auto in1 = at::randint(1, 10, {1, 3, 4}, {at::kCUDA});

  auto params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto jit_results = torch_tensorrt::tests::util::RunGraph(g, params, {in1});

  params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto trt_results = torch_tensorrt::tests::util::RunGraphEngineDynamic(g, params, {in1});

  ASSERT_TRUE(
      torch_tensorrt::tests::util::almostEqual(jit_results[0], trt_results[0].reshape_as(jit_results[0]), 2e-6));
}

TEST(Converters, ATenConstantPad2dTensorConvertsCorrectly) {
  const auto graph = R"IR(
      graph(%0 : Tensor):
        %1 : int[] = prim::Constant[value=[2, 3, 2, 3]]()
        %2 : Scalar = prim::Constant[value=2]()
        %3 : Tensor = aten::constant_pad_nd(%0, %1, %2)
        return (%3))IR";

  auto g = std::make_shared<torch::jit::Graph>();
  torch::jit::parseIR(graph, g.get());

  auto in1 = at::randint(1, 10, {1, 3, 4, 5}, {at::kCUDA});

  auto params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto jit_results = torch_tensorrt::tests::util::RunGraph(g, params, {in1});

  params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto trt_results = torch_tensorrt::tests::util::RunGraphEngine(g, params, {in1});

  ASSERT_TRUE(
      torch_tensorrt::tests::util::almostEqual(jit_results[0], trt_results[0].reshape_as(jit_results[0]), 2e-6));
}

TEST(Converters, ATenConstantPad2dRightBottomZeroTensorConvertsCorrectly) {
  const auto graph = R"IR(
      graph(%0 : Tensor):
        %1 : int[] = prim::Constant[value=[2, 0, 2, 0]]()
        %2 : Scalar = prim::Constant[value=2]()
        %3 : Tensor = aten::constant_pad_nd(%0, %1, %2)
        return (%3))IR";

  auto g = std::make_shared<torch::jit::Graph>();
  torch::jit::parseIR(graph, g.get());

  auto in1 = at::randint(1, 10, {1, 3, 4, 5}, {at::kCUDA});

  auto params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto jit_results = torch_tensorrt::tests::util::RunGraph(g, params, {in1});

  params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto trt_results = torch_tensorrt::tests::util::RunGraphEngine(g, params, {in1});

  ASSERT_TRUE(
      torch_tensorrt::tests::util::almostEqual(jit_results[0], trt_results[0].reshape_as(jit_results[0]), 2e-6));
}

TEST(Converters, ATenConstantPad2dTensorConvertsCorrectlyWithDynamic) {
  const auto graph = R"IR(
      graph(%0 : Tensor):
        %1 : int[] = prim::Constant[value=[2, 3, 2, 3]]()
        %2 : Scalar = prim::Constant[value=2]()
        %3 : Tensor = aten::constant_pad_nd(%0, %1, %2)
        return (%3))IR";

  auto g = std::make_shared<torch::jit::Graph>();
  torch::jit::parseIR(graph, g.get());

  auto in1 = at::randint(1, 10, {1, 3, 4, 5}, {at::kCUDA});

  auto params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto jit_results = torch_tensorrt::tests::util::RunGraph(g, params, {in1});

  params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto trt_results = torch_tensorrt::tests::util::RunGraphEngineDynamic(g, params, {in1});

  ASSERT_TRUE(
      torch_tensorrt::tests::util::almostEqual(jit_results[0], trt_results[0].reshape_as(jit_results[0]), 2e-6));
}

TEST(Converters, ATenConstantPad3dTensorConvertsCorrectly) {
  const auto graph = R"IR(
      graph(%0 : Tensor):
        %1 : int[] = prim::Constant[value=[2, 3, 2, 3, 1, 4]]()
        %2 : Scalar = prim::Constant[value=2]()
        %3 : Tensor = aten::constant_pad_nd(%0, %1, %2)
        return (%3))IR";

  auto g = std::make_shared<torch::jit::Graph>();
  torch::jit::parseIR(graph, g.get());

  auto in1 = at::randint(1, 10, {1, 3, 4, 5, 3}, {at::kCUDA});

  auto params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto jit_results = torch_tensorrt::tests::util::RunGraph(g, params, {in1});

  params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto trt_results = torch_tensorrt::tests::util::RunGraphEngine(g, params, {in1});

  ASSERT_TRUE(
      torch_tensorrt::tests::util::almostEqual(jit_results[0], trt_results[0].reshape_as(jit_results[0]), 2e-6));
}

TEST(Converters, ATenConstantPad3dRightBottomBackZeroTensorConvertsCorrectly) {
  const auto graph = R"IR(
      graph(%0 : Tensor):
        %1 : int[] = prim::Constant[value=[2, 0, 2, 0, 1, 0]]()
        %2 : Scalar = prim::Constant[value=2]()
        %3 : Tensor = aten::constant_pad_nd(%0, %1, %2)
        return (%3))IR";
  auto g = std::make_shared<torch::jit::Graph>();
  torch::jit::parseIR(graph, g.get());

  auto in1 = at::randint(1, 10, {1, 3, 4, 5, 3}, {at::kCUDA});

  auto params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto jit_results = torch_tensorrt::tests::util::RunGraph(g, params, {in1});

  params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto trt_results = torch_tensorrt::tests::util::RunGraphEngine(g, params, {in1});

  ASSERT_TRUE(
      torch_tensorrt::tests::util::almostEqual(jit_results[0], trt_results[0].reshape_as(jit_results[0]), 2e-6));
}

TEST(Converters, ATenConstantPad3dTensorConvertsCorrectlyWithDynamic) {
  const auto graph = R"IR(
      graph(%0 : Tensor):
        %1 : int[] = prim::Constant[value=[2, 3, 2, 3, 1, 4]]()
        %2 : Scalar = prim::Constant[value=2]()
        %3 : Tensor = aten::constant_pad_nd(%0, %1, %2)
        return (%3))IR";

  auto g = std::make_shared<torch::jit::Graph>();
  torch::jit::parseIR(graph, g.get());

  auto in1 = at::randint(1, 10, {1, 3, 4, 5, 3}, {at::kCUDA});

  auto params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto jit_results = torch_tensorrt::tests::util::RunGraph(g, params, {in1});

  params = torch_tensorrt::core::ir::get_static_params(g->inputs(), {});
  auto trt_results = torch_tensorrt::tests::util::RunGraphEngineDynamic(g, params, {in1});

  ASSERT_TRUE(
      torch_tensorrt::tests::util::almostEqual(jit_results[0], trt_results[0].reshape_as(jit_results[0]), 2e-6));
}