/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "glow/Base/Tensor.h"
#include "glow/ExecutionEngine/ExecutionEngine.h"
#include "glow/Graph/Graph.h"
#include "glow/Graph/Nodes.h"
#include "glow/IR/IR.h"
#include "glow/IR/IRBuilder.h"
#include "glow/IR/Instrs.h"

#include "gtest/gtest.h"

using namespace glow;
using llvm::cast;

class GradCheckBase : public ::testing::TestWithParam<BackendKind> {
public:
  ExecutionEngine EE_{GetParam()};
};

class InterpreterGrad : public GradCheckBase {};
class GradCheck : public GradCheckBase {};

/// \returns the regression loss for the tensor \p X with regard to \p Y.
float computeL2Loss(Tensor *X, Tensor *Y) {
  assert(X->dims() == Y->dims() && "Invalid input dims");
  auto xH = X->getHandle<>();
  auto yH = Y->getHandle<>();
  float loss = 0;

  for (size_t i = 0, e = X->size(); i < e; i++) {
    float dy = (xH.raw(i) - yH.raw(i));
    loss += 0.5 * dy * dy;
  }

  return loss;
}

/// \returns the error when comparing two grads: absolute or relative.
float gradDiff(float G1, float G2) {
  return std::min(std::abs(G1 - G2), std::abs(G1 - G2) / std::abs(G1 + G2 + 1));
}

Placeholder *getGrad(const VariableGradientsList &grads, Placeholder *V) {
  for (auto &p : grads) {
    if (p.first == V) {
      return cast<Placeholder>(p.second);
    }
  }
  return nullptr;
}

void allocateGrads(Context &ctx, const VariableGradientsList &grads) {
  for (auto &p : grads) {
    auto grad = cast<Placeholder>(p.second);
    ctx.allocate(grad);
  }
}

/// Performs gradient check by comparing analytical and numerical gradients.
/// Numeric grad is calculated based on: f(x-delta) and f(x+delta) values.
/// Analytical grad is based on the gradient output calculated during back
/// propagation.
///
/// \param EE Execution engine to compile/run network.
/// \param ctx Execution context.
/// \param result Node that contains result of f(x).
/// \param inputVar Placeholder which gradient is assessed.
/// \param expVar Placeholder with expected value, only used during the
/// training. \param inputs Tensor for \p inputVar placeholder. \param outputs
/// Tensor for \p expVar placeholder. \param allowedError allowed delta between
/// analytical and numerical gradients.
void performGradCheck(ExecutionEngine &EE, Context &ctx, SaveNode *result,
                      Placeholder *inputVar, Placeholder *expVar,
                      Tensor *inputs, Tensor *outputs, float delta,
                      float allowedError) {
  TrainingConfig TC;

  auto &F = *EE.getModule().getFunction("main");

  // Allocate result, inputVar and expVar.
  auto resultTensor = ctx.allocate(result->getPlaceholder());
  ctx.allocate(inputVar);
  ctx.allocate(expVar);

  // This variable records the number of the next sample to be used for
  // training.
  size_t sampleCounter = 0;

  // Create a function that trains the network.
  Function *TF = glow::differentiate(&F, TC);
  EE.compile(CompilationMode::Train, TF, ctx);

  // The network might have variables, other than inputVar and expVar.
  // Train the network until other variables reach some stable local minimum.
  runBatch(EE, ctx, 300, sampleCounter, {inputVar, expVar}, {inputs, outputs});

  // Create a version of the network that records the gradients to some side
  // table instead of updating them.
  VariableGradientsList varGrads;
  Function *recordNet = glow::differentiate(&F, TC, "record", &varGrads);
  allocateGrads(ctx, varGrads);
  EE.compile(CompilationMode::Train, recordNet, ctx);

  // Clear the gradients of the first layer.
  auto gradVar = getGrad(varGrads, inputVar);
  auto gradVarTensor = ctx.get(gradVar);
  gradVarTensor->zero();

  // Train the network just once to record the values of gradient for inputVar.
  runBatch(EE, ctx, 1, sampleCounter, {inputVar, expVar}, {inputs, outputs});

  // Compile the original network in inference mode.
  EE.compile(CompilationMode::Infer, &F, ctx);

  auto analyticalGradsH = gradVarTensor->getHandle();
  auto inputsH = inputs->getHandle<>();
  for (size_t i = 0; i < analyticalGradsH.size(); i++) {
    auto old = inputsH.raw(i);
    // Calculate f(x+e):
    inputsH.raw(i) = old + delta;
    updateVariables(ctx, {inputVar}, {inputs});
    EE.run();
    auto plusLoss = computeL2Loss(outputs, resultTensor);

    // Calculate f(x-e):
    inputsH.raw(i) = old - delta;
    updateVariables(ctx, {inputVar}, {inputs});
    EE.run();

    auto minusLoss = computeL2Loss(outputs, resultTensor);

    // Restore value back.
    inputsH.raw(i) = old;

    auto numericGrad = (plusLoss - minusLoss) / (2 * delta);
    auto analyticalGrad = analyticalGradsH.raw(i);

    auto err = gradDiff(analyticalGrad, numericGrad);

    // Make sure that the analytical and numerical gradients agree.
    EXPECT_LE(err, allowedError);
  }
}

TEST_P(InterpreterGrad, gradientCheckFCConcatRELU) {
  Context ctx;
  size_t numInputElem = 20;
  size_t numOutputElem = 10;

  auto &mod = EE_.getModule();
  Function *F = mod.createFunction("main");
  auto *A =
      mod.createPlaceholder(ElemKind::FloatTy, {1, numInputElem}, "A", false);
  auto *Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem},
                                    "exp", false);

  Node *FA = F->createFullyConnected("fc1", A, numOutputElem / 2);
  FA = F->createRELU("relu1", FA);

  auto *B =
      mod.createPlaceholder(ElemKind::FloatTy, {1, numInputElem}, "B", true);
  ctx.allocate(B);
  Node *FB = F->createFullyConnected("fc2", B, numOutputElem / 2);
  FB = F->createRELU("relu2", FB);

  Node *O = F->createConcat("concat", {FA, FB}, 1);
  O = F->createRegression("reg", O, Exp);
  auto *result = F->createSave(ctx, "ret", O);

  Tensor inputs(ElemKind::FloatTy, {{1, numInputElem}});
  Tensor outputs(ElemKind::FloatTy, {{1, numOutputElem}});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();

  inputsH.initXavier(1, mod.getPRNG());
  outputsH.initXavier(1, mod.getPRNG());

  performGradCheck(EE_, ctx, result, A, Exp, &inputs, &outputs, 0.0001, 0.001);
}

static void gradientCheckGroupConv(size_t depth, size_t group,
                                   ExecutionEngine &EE_) {
  Context ctx;
  size_t numDim = 10;

  auto &mod = EE_.getModule();
  Function *F = mod.createFunction("main");
  auto *A = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim, numDim, depth},
                                  "A", false);
  auto *Ex = mod.createPlaceholder(
      ElemKind::FloatTy, {1, numDim + 1, numDim + 1, depth}, "exp", false);

  Node *O = F->createConv("conv", A, depth, 2, 1, 1, group);
  O = F->createRegression("reg", O, Ex);
  auto *result = F->createSave(ctx, "ret", O);

  Tensor inputs(ElemKind::FloatTy, {1, numDim, numDim, depth});
  Tensor outputs(ElemKind::FloatTy, {1, numDim + 1, numDim + 1, depth});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();

  inputsH.randomize(-1, 1, mod.getPRNG());
  outputsH.randomize(-1, 1, mod.getPRNG());

  performGradCheck(EE_, ctx, result, A, Ex, &inputs, &outputs, 0.001, 0.04);
}

TEST_P(GradCheck, gradientCheckConv) { gradientCheckGroupConv(1, 1, EE_); }

TEST_P(GradCheck, gradientCheckDepthwiseConv) {
  gradientCheckGroupConv(4, 4, EE_);
}

TEST_P(GradCheck, gradientCheckGroupConv) { gradientCheckGroupConv(4, 2, EE_); }

TEST_P(InterpreterGrad, gradientCheckAvgPool) {
  Context ctx;
  size_t numDim = 10;
  size_t numOutputElem = 10;

  auto &mod = EE_.getModule();
  Function *F = mod.createFunction("main");
  auto *A = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim, numDim, 1},
                                  "A", false);
  auto *Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem},
                                    "Exp", false);

  Node *O = F->createAvgPool("pool", A, 3, 3, 1);
  O = F->createTanh("tanh", O);
  O = F->createFullyConnected("fc", O, numOutputElem);
  O = F->createRegression("reg", O, Exp);
  auto *result = F->createSave(ctx, "ret", O);

  Tensor inputs(ElemKind::FloatTy, {1, numDim, numDim, 1});
  Tensor outputs(ElemKind::FloatTy, {1, numOutputElem});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();

  inputsH.initXavier(1, mod.getPRNG());
  outputsH.initXavier(1, mod.getPRNG());

  performGradCheck(EE_, ctx, result, A, Exp, &inputs, &outputs, 0.001, 0.004);
}

TEST_P(InterpreterGrad, gradientCheckBatchNorm) {
  Context ctx;
  size_t numDim = 5;
  size_t numOutputElem = numDim * numDim * 3;

  auto &mod = EE_.getModule();
  Function *F = mod.createFunction("main");
  auto *A = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim, numDim, 3},
                                  "A", false);
  auto *Ex = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem}, "exp",
                                   false);

  Node *O = F->createBatchNormalization(ctx, "batch", A, 3, 0.0001, 0.9);
  O = F->createReshape("reshape", O, {1, numDim * numDim * 3});
  O = F->createRegression("reg", O, Ex);
  auto result = F->createSave(ctx, "ret", O);

  Tensor inputs(ElemKind::FloatTy, {1, numDim, numDim, 3});
  Tensor outputs(ElemKind::FloatTy, {1, numOutputElem});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();

  inputsH.initXavier(1, mod.getPRNG());
  outputsH.initXavier(1, mod.getPRNG());

  for (int i = 0, e = inputsH.size(); i < e; i++) {
    inputsH.raw(i) *= 6;
    inputsH.raw(i) += 4;
  }

  performGradCheck(EE_, ctx, result, A, Ex, &inputs, &outputs, 0.001, 0.004);
}

TEST_P(InterpreterGrad, gradientCheckArithmeticDiv) {
  // The test creates a net: A / B = Exp. Where A is trainable weight,
  // B and Exp are external data (initialized randomly once). SGD will find
  // correct value for A, and then gradient check will be performed.
  Context ctx;
  size_t numDim = 10;

  auto &mod = EE_.getModule();
  Function *F = mod.createFunction("main");
  auto *A = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "A", true);
  auto *B = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "B", false);
  auto *Exp =
      mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "exp", false);

  auto *ATensor = ctx.allocate(A);
  ATensor->init(Tensor::InitKind::Xavier, 1.0, mod.getPRNG());

  Node *O = F->createDiv("div", A, B);
  O = F->createRegression("reg", O, Exp);
  auto *result = F->createSave(ctx, "ret", O);

  Tensor BValues(ElemKind::FloatTy, {1, numDim});
  Tensor ExpValues(ElemKind::FloatTy, {1, numDim});
  // Random values are in the range, so that all intermediate computations are
  // not too small and not too large.
  BValues.getHandle().randomize(0.1, 1, mod.getPRNG());
  ExpValues.getHandle().randomize(0.1, 1, mod.getPRNG());

  performGradCheck(EE_, ctx, result, B, Exp, &BValues, &ExpValues, 0.0001,
                   0.001);
}

TEST_P(InterpreterGrad, gradientCheckArithmetic) {
  Context ctx;
  size_t numDim = 20;

  auto &mod = EE_.getModule();
  Function *F = mod.createFunction("main");
  auto *A = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "A", false);
  auto *B = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "B", false);
  auto *C = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "C", false);
  auto *D = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "D", false);
  auto *E = mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "E", false);
  ctx.allocate(B);
  ctx.allocate(C);
  ctx.allocate(D);

  // Randomize E to avoid div by zero.
  auto *ETensor = ctx.allocate(E);
  ETensor->getHandle().initXavier(1, mod.getPRNG());

  auto *Exp =
      mod.createPlaceholder(ElemKind::FloatTy, {1, numDim}, "exp", false);

  Node *O = F->createMul("mul", A, B);
  O = F->createAdd("add", O, C);
  O = F->createSub("sub", D, O);
  O = F->createDiv("div", O, E);
  O = F->createRegression("reg", O, Exp);
  auto *result = F->createSave(ctx, "ret", O);

  Tensor inputs(ElemKind::FloatTy, {1, numDim});
  Tensor outputs(ElemKind::FloatTy, {1, numDim});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();

  inputsH.initXavier(1, mod.getPRNG());
  outputsH.initXavier(1, mod.getPRNG());

  performGradCheck(EE_, ctx, result, A, Exp, &inputs, &outputs, 0.01, 0.004);
}

TEST_P(InterpreterGrad, gradientCheckFCConcatTanh) {
  // Using the same gradient check test setup as gradientCheck_FC_Concat_RELU
  Context ctx;
  size_t numInputElem = 20;
  size_t numOutputElem = 10;

  auto &mod = EE_.getModule();
  Function *F = mod.createFunction("main");
  auto *A =
      mod.createPlaceholder(ElemKind::FloatTy, {1, numInputElem}, "A", false);
  auto *Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem},
                                    "Exp", false);

  Node *FA = F->createFullyConnected("fc", A, numOutputElem);
  FA = F->createTanh("tanh", FA);
  FA = F->createRegression("reg", FA, Exp);
  auto *result = F->createSave(ctx, "ret", FA);

  Tensor inputs(ElemKind::FloatTy, {{1, numInputElem}});
  Tensor outputs(ElemKind::FloatTy, {{1, numOutputElem}});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();

  inputsH.initXavier(1, mod.getPRNG());
  outputsH.initXavier(1, mod.getPRNG());

  performGradCheck(EE_, ctx, result, A, Exp, &inputs, &outputs, 0.0001, 0.001);
}

TEST_P(InterpreterGrad, gradientCheckFC) {
  Context ctx;
  size_t numInputElem = 20;
  size_t numOutputElem = 10;

  auto &mod = EE_.getModule();
  Function *F = mod.createFunction("main");
  auto *A =
      mod.createPlaceholder(ElemKind::FloatTy, {1, numInputElem}, "A", false);
  auto *Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem},
                                    "Exp", false);

  Node *FA = F->createFullyConnected("fc", A, numOutputElem);
  FA = F->createRegression("reg", FA, Exp);
  auto *result = F->createSave(ctx, "ret", FA);

  Tensor inputs(ElemKind::FloatTy, {{1, numInputElem}});
  Tensor outputs(ElemKind::FloatTy, {{1, numOutputElem}});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();

  inputsH.initXavier(1, mod.getPRNG());
  outputsH.initXavier(1, mod.getPRNG());

  performGradCheck(EE_, ctx, result, A, Exp, &inputs, &outputs, 0.0001, 0.0001);
}

TEST_P(InterpreterGrad, gradientCheckSigmoid) {
  Context ctx;
  size_t numInputElem = 20;
  size_t numOutputElem = 10;

  auto &mod = EE_.getModule();
  Function *F = mod.createFunction("main");
  auto *A =
      mod.createPlaceholder(ElemKind::FloatTy, {1, numInputElem}, "A", false);
  auto *Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem},
                                    "Exp", false);
  auto *result = F->createSave(ctx, "ret", A);
  ctx.allocate(result->getPlaceholder());

  Node *FA = F->createSigmoid("sig", Exp);
  FA = F->createRegression("reg", FA, Exp);
  result = F->createSave(ctx, "ret", FA);

  Tensor inputs(ElemKind::FloatTy, {{1, numInputElem}});
  Tensor outputs(ElemKind::FloatTy, {{1, numOutputElem}});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();

  inputsH.initXavier(1, mod.getPRNG());
  outputsH.initXavier(1, mod.getPRNG());

  performGradCheck(EE_, ctx, result, A, Exp, &inputs, &outputs, 0.0001, 0.001);
}

TEST_P(InterpreterGrad, gradientCheckRelu) {
  Context ctx;
  size_t numInputElem = 20;
  size_t numOutputElem = 10;

  auto &mod = EE_.getModule();
  Function *F = mod.createFunction("main");
  auto *A =
      mod.createPlaceholder(ElemKind::FloatTy, {1, numInputElem}, "A", false);
  auto *Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem},
                                    "Exp", false);
  auto *result = F->createSave(ctx, "ret", A);
  ctx.allocate(result->getPlaceholder());

  Node *FA = F->createRELU("relu", Exp);
  FA = F->createRegression("reg", FA, Exp);
  result = F->createSave(ctx, "ret", FA);

  Tensor inputs(ElemKind::FloatTy, {{1, numInputElem}});
  Tensor outputs(ElemKind::FloatTy, {{1, numOutputElem}});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();

  inputsH.initXavier(1, mod.getPRNG());
  outputsH.initXavier(1, mod.getPRNG());

  performGradCheck(EE_, ctx, result, A, Exp, &inputs, &outputs, 0.0001, 0.001);
}

TEST_P(InterpreterGrad, gradientCheckTranspose) {
  // Using the same gradient check test setup as gradientCheck_FC_Concat_RELU
  Context ctx;
  size_t numOutputElem = 10;

  auto &mod = EE_.getModule();
  Function *F = mod.createFunction("main");
  auto *A =
      mod.createPlaceholder(ElemKind::FloatTy, {1, 5, 10, 5}, "input", false);
  auto *Exp = mod.createPlaceholder(ElemKind::FloatTy, {1, numOutputElem},
                                    "exp", false);
  Node *TA = F->createTranspose("transpose", A, NHWC2NCHW);
  TA = F->createFullyConnected("fc", TA, numOutputElem);
  TA = F->createRegression("regress", TA, Exp);
  auto *result = F->createSave(ctx, "ret", TA);

  Tensor inputs(ElemKind::FloatTy, {1, 5, 10, 5});
  Tensor outputs(ElemKind::FloatTy, {1, numOutputElem});

  auto inputsH = inputs.getHandle<>();
  auto outputsH = outputs.getHandle<>();

  inputsH.randomize(-1, 1, mod.getPRNG());
  outputsH.randomize(-1, 1, mod.getPRNG());

  performGradCheck(EE_, ctx, result, A, Exp, &inputs, &outputs, 0.0001, 0.001);
}

TEST_P(InterpreterGrad, gradientCheckCrossEntropyLoss) {
  const size_t batchSize = 6;
  const int testSamples = 5;
  const float stepSize = 1e-4;
  const float delta = 0.015;
  TrainingConfig TC;
  Context ctx;

  auto &mod = EE_.getModule();
  Function *F = mod.createFunction("main");
  auto *P =
      mod.createPlaceholder(ElemKind::FloatTy, {batchSize, 4}, "P", false);
  ctx.allocate(P);
  auto *Y =
      mod.createPlaceholder(ElemKind::Int64ITy, {batchSize}, "Labels", false);
  ctx.allocate(Y);
  Node *CE = F->createCrossEntropyLoss("celoss", P, Y);
  auto *result = F->createSave(ctx, "ret", CE);
  auto *LTensor = ctx.allocate(result->getPlaceholder());

  Tensor inputs(ElemKind::FloatTy, {batchSize, 4});
  Tensor outputs(ElemKind::Int64ITy, {batchSize});

  auto inputsH = inputs.getHandle();
  auto outputsH = outputs.getHandle<int64_t>();

  inputsH.randomize(0.0, 1.0, mod.getPRNG());
  outputsH.at({0}) = 2;
  outputsH.at({1}) = 0;
  outputsH.at({2}) = 1;

  VariableGradientsList varGrads;
  Function *TF = glow::differentiate(F, TC, "record", &varGrads);
  allocateGrads(ctx, varGrads);
  EE_.compile(CompilationMode::Train, TF, ctx);

  auto *gradPlaceholder = getGrad(varGrads, P);
  auto gradTensorHandle = ctx.get(gradPlaceholder)->getHandle();

  for (int i = 0; i < testSamples; ++i) {
    inputsH.randomize(0.0, 1.0, mod.getPRNG());
    for (size_t j = 0; j < inputsH.size(); ++j) {
      updateVariables(ctx, {P, Y}, {&inputs, &outputs});
      EE_.run();
      LTensor->zero();
      auto x = inputsH.raw(j);
      auto g = gradTensorHandle.raw(j);
      inputsH.raw(j) = x + stepSize;
      updateVariables(ctx, {P, Y}, {&inputs, &outputs});
      EE_.run();
      auto lp = LTensor->getHandle().raw(0);
      inputsH.raw(j) = x - stepSize;
      LTensor->zero();
      updateVariables(ctx, {P, Y}, {&inputs, &outputs});
      EE_.run();
      auto lm = LTensor->getHandle().raw(0);
      auto diff = (lp - lm) / (2 * stepSize);
      inputsH.raw(j) = x;
      updateVariables(ctx, {P, Y}, {&inputs, &outputs});
      EE_.run();
      EXPECT_NEAR(diff, g, delta);
    }
  }
}

INSTANTIATE_TEST_CASE_P(Interpreter, InterpreterGrad,
                        ::testing::Values(BackendKind::Interpreter));

INSTANTIATE_TEST_CASE_P(Interpreter, GradCheck,
                        ::testing::Values(BackendKind::Interpreter));

#ifdef GLOW_WITH_CPU
INSTANTIATE_TEST_CASE_P(JIT, GradCheck, ::testing::Values(BackendKind::CPU));
#endif // GLOW_WITH_CPU
