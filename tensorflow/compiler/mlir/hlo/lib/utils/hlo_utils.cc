/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "mlir-hlo/utils/hlo_utils.h"

#include <numeric>

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Attributes.h"

namespace mlir {
namespace hlo {

static constexpr size_t kPaddingSize = 64;

DenseIntElementsAttr getBroadcastDimensionsAttr(Builder* b, Value x, Value y,
                                                bool allowEmpty) {
  TensorType xType = x.getType().dyn_cast<RankedTensorType>();
  TensorType yType = y.getType().dyn_cast<RankedTensorType>();
  if (!xType || !yType) return {};
  if (allowEmpty && xType == yType) return {};

  // If the shapes have the same rank, then there is nothing to do.
  auto xRank = xType.getRank(), yRank = yType.getRank();
  if (allowEmpty && xRank == yRank) return {};

  // Otherwise if the ranks of the inputs don't match, TensorFlow automatically
  // reshapes the smaller by padding with dimensions of size 1 as a prefix. In
  // other words to pad a 5-vector to a 3-dimensional tensor it is reshaped to
  // have shape [1,1,5]. XLA's automatic broadcast code is able to broadcast
  // from lower to higher rank, but doesn't assume you want to pad as a prefix
  // of the dimensions, and instead needs to be told which dimensions of the
  // higher rank tensor to match to the lower rank tensor.
  auto maxRank = std::max(xRank, yRank);
  auto minRank = std::min(xRank, yRank);

  // Match the lower rank tensor along the larger-numbered dimensions of the
  // higher rank tensor.
  SmallVector<int64_t, 4> broadcastDimensions(minRank);
  std::iota(broadcastDimensions.begin(), broadcastDimensions.end(),
            maxRank - minRank);

  RankedTensorType type =
      RankedTensorType::get({minRank}, b->getIntegerType(64));
  return DenseIntElementsAttr::get(type, broadcastDimensions);
}

DenseElementsAttr GetScalarOfType(Type ty, int64_t rawValue) {
  RankedTensorType scalarTy = RankedTensorType::get({}, ty);

  if (auto floatTy = ty.dyn_cast<FloatType>()) {
    APFloat value(floatTy.getFloatSemantics(), rawValue);
    return DenseElementsAttr::get(scalarTy, value);
  }
  if (auto intTy = ty.dyn_cast<IntegerType>()) {
    APInt value(intTy.getWidth(), static_cast<int64_t>(rawValue),
                /*isSigned=*/true);
    return DenseElementsAttr::get(scalarTy, value);
  }
  if (auto complexTy = ty.dyn_cast<ComplexType>()) {
    if (auto floatTy = complexTy.getElementType().cast<FloatType>()) {
      APFloat real(floatTy.getFloatSemantics(), rawValue);
      APFloat imag = APFloat::getZero(floatTy.getFloatSemantics());
      return DenseElementsAttr::get(scalarTy,
                                    std::complex<APFloat>(real, imag));
    }
  }
  llvm_unreachable("unsupported type");
}

DenseElementsAttr GetScalarNegZeroOfType(Type ty) {
  RankedTensorType scalarTy = RankedTensorType::get({}, ty);

  if (auto floatTy = ty.dyn_cast<FloatType>()) {
    APFloat negZero =
        APFloat::getZero(floatTy.getFloatSemantics(), /*Negative=*/true);
    return DenseElementsAttr::get(scalarTy, negZero);
  }
  if (auto intTy = ty.dyn_cast<IntegerType>()) {
    return DenseElementsAttr::get(scalarTy, APInt::getZero(intTy.getWidth()));
  }
  if (auto complexTy = ty.dyn_cast<ComplexType>()) {
    if (auto floatTy = complexTy.getElementType().cast<FloatType>()) {
      APFloat negZero =
          APFloat::getZero(floatTy.getFloatSemantics(), /*Negative=*/true);
      return DenseElementsAttr::get(scalarTy,
                                    std::complex<APFloat>(negZero, negZero));
    }
  }
  llvm_unreachable("unsupported type");
}

static APFloat getScalarLimitOfFloatType(FloatType floatTy, ScalarLimit limit) {
  auto& semantics = floatTy.getFloatSemantics();
  switch (limit) {
    case kLowest:
      return APFloat::getLargest(semantics, /*negative=*/true);
    case kInfinityLowest:
      return APFloat::getInf(semantics, /*negative=*/true);
    case kMax:
      return APFloat::getLargest(semantics, /*negative=*/false);
    case kInfinityMax:
      return APFloat::getInf(semantics, /*negative=*/false);
  }
  llvm_unreachable("invalid limit");
}

// Returns a scalar value for the given integer type.
//
// The argument 'scalar' describes which scalar value to return. `integer_value`
// is used to specify the integer value for kInteger. For any other scalar,
// integer_value is ignored.
static APInt getScalarLimitOfIntegerType(IntegerType integerTy,
                                         ScalarLimit limit) {
  unsigned width = integerTy.getWidth();
  bool isBool = (width == 1);
  switch (limit) {
    case kLowest:
    case kInfinityLowest:
      if (integerTy.isUnsigned() || isBool) {
        return APInt::getMinValue(width);
      } else {
        return APInt::getSignedMinValue(width);
      }

    case kMax:
    case kInfinityMax:
      if (integerTy.isUnsigned() || isBool) {
        return APInt::getMaxValue(width);
      } else {
        return APInt::getSignedMaxValue(width);
      }
  }
  llvm_unreachable("invalid limit");
}

DenseElementsAttr GetScalarLimitOfType(Type ty, ScalarLimit limit) {
  RankedTensorType scalarTy = RankedTensorType::get({}, ty);
  if (auto floatTy = ty.dyn_cast<FloatType>()) {
    return DenseElementsAttr::get(scalarTy,
                                  getScalarLimitOfFloatType(floatTy, limit));
  }
  if (auto integerTy = ty.dyn_cast<IntegerType>()) {
    return DenseElementsAttr::get(
        scalarTy, getScalarLimitOfIntegerType(integerTy, limit));
  }
  llvm_unreachable("unsupported type");
}

std::string LmhloToMhloOpName(llvm::StringRef opName,
                              mlir::MLIRContext* context) {
  assert(opName.startswith("lmhlo.") && "Expected an LMHLO op");

  if (opName == "lmhlo.dot") {
    return "mhlo.dot_general";
  }

  if (opName == "lmhlo.dynamic_slice") {
    return "mhlo.dynamic-slice";
  }

  std::string mhloOpName(opName.drop_front(1));
  if (context->isOperationRegistered(mhloOpName)) return mhloOpName;
  return "";
}

bool IsSequenceStartingWith0(Attribute attr) {
  DenseIntElementsAttr denseAttr = attr.dyn_cast<DenseIntElementsAttr>();
  for (int64_t i = 0, e = denseAttr.getNumElements(); i < e; ++i)
    if (denseAttr.getValues<APInt>()[i].getSExtValue() != i) return false;
  return true;
}

int64_t getArgumentIndex(mlir::func::FuncOp op, Value value) {
  BlockArgument arg = value.dyn_cast<BlockArgument>();
  if (!arg || arg.getOwner() != &op.front()) return -1;
  return arg.getArgNumber();
}

/// Computes the memory usage of the given allocations.
std::pair<size_t, size_t> computeMemory(const std::vector<Value>& allocs) {
  size_t totalSize = 0;
  size_t allocCounter = 0;
  for (const Value alloc : allocs) {
    auto shape = alloc.getType().cast<ShapedType>();
    size_t shapeBytes = llvm::divideCeil(shape.getSizeInBits(), 8);
    size_t alignFactor = llvm::divideCeil(shapeBytes, kPaddingSize);
    size_t size = alignFactor * kPaddingSize;
    totalSize += size;
    allocCounter++;
  }
  return std::make_pair(totalSize, allocCounter);
}

}  // namespace hlo
}  // namespace mlir
