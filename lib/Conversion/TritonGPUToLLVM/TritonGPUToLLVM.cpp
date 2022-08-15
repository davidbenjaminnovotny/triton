#include "triton/Conversion/TritonGPUToLLVM/TritonGPUToLLVM.h"

#include "../PassDetail.h"
#include "mlir/Conversion/ArithmeticToLLVM/ArithmeticToLLVM.h"
#include "mlir/Conversion/GPUToNVVM/GPUToNVVMPass.h"
#include "mlir/Conversion/LLVMCommon/LoweringOptions.h"
#include "mlir/Conversion/LLVMCommon/Pattern.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Transforms/DialectConversion.h"
#include "triton/Conversion/TritonToTritonGPU/TritonToTritonGPU.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "triton/Dialect/TritonGPU/IR/Dialect.h"
#include <numeric>

using namespace mlir;
using namespace mlir::triton;
using ::mlir::triton::gpu::TritonGPUBlockedEncodingAttr;
using ::mlir::triton::gpu::TritonGPUMmaEncodingAttr;
using ::mlir::triton::gpu::TritonGPUSharedEncodingAttr;

namespace mlir {
namespace LLVM {

static StringRef getStructAttrsAttrName() { return "llvm.struct_attrs"; }

} // namespace LLVM
} // namespace mlir

namespace {

class TritonGPUToLLVMTypeConverter;

// The following code are borrowed from mlir project including the following
// functions or classes:
// - filterFuncAttributes
// - ConvertOpToLLVMPattern
// - FuncOpConversion
//
// The code are hidden in the CPP files in MLIR repo, and we can't call them
// directly. I found such code snippets are refactored and added to LLVMCommon
// in the latest MLIR code, but the v14.0.0 version currentlly used in Triton
// doesn't contain the code.
// TODO(Superjomn) Remove the code when mlir v15.0 is included.
//
// The original code:
// https://github.com/llvm/llvm-project/blob/f28c006a5895fc0e329fe15fead81e37457cb1d1/mlir/lib/Conversion/StandardToLLVM/StandardToLLVM.cpp#L219
// All the rights are reserved by LLVM community.

/// Only retain those attributes that are not constructed by
/// `LLVMFuncOp::build`. If `filterArgAttrs` is set, also filter out argument
/// attributes.
static void filterFuncAttributes(ArrayRef<NamedAttribute> attrs,
                                 bool filterArgAttrs,
                                 SmallVectorImpl<NamedAttribute> &result) {
  for (const auto &attr : attrs) {
    if (attr.getName() == SymbolTable::getSymbolAttrName() ||
        attr.getName() == FunctionOpInterface::getTypeAttrName() ||
        attr.getName() == "std.varargs" ||
        (filterArgAttrs &&
         attr.getName() == FunctionOpInterface::getArgDictAttrName()))
      continue;
    result.push_back(attr);
  }
}

struct FuncOpConversionBase : public ConvertOpToLLVMPattern<FuncOp> {
protected:
  using ConvertOpToLLVMPattern<FuncOp>::ConvertOpToLLVMPattern;

  // Convert input FuncOp to LLVMFuncOp by using the LLVMTypeConverter provided
  // to this legalization pattern.
  LLVM::LLVMFuncOp
  convertFuncOpToLLVMFuncOp(FuncOp funcOp,
                            ConversionPatternRewriter &rewriter) const {
    // Convert the original function arguments. They are converted using the
    // LLVMTypeConverter provided to this legalization pattern.
    auto varargsAttr = funcOp->getAttrOfType<BoolAttr>("std.varargs");
    TypeConverter::SignatureConversion result(funcOp.getNumArguments());
    auto llvmType = getTypeConverter()->convertFunctionSignature(
        funcOp.getType(), varargsAttr && varargsAttr.getValue(), result);
    assert(llvmType);
    if (!llvmType)
      return nullptr;

    // Propagate argument attributes to all converted arguments obtained after
    // converting a given original argument.
    SmallVector<NamedAttribute, 4> attributes;
    filterFuncAttributes(funcOp->getAttrs(), /*filterArgAttrs=*/true,
                         attributes);
    if (ArrayAttr argAttrDicts = funcOp.getAllArgAttrs()) {
      SmallVector<Attribute, 4> newArgAttrs(
          llvmType.cast<LLVM::LLVMFunctionType>().getNumParams());
      for (unsigned i = 0, e = funcOp.getNumArguments(); i < e; ++i) {
        auto mapping = result.getInputMapping(i);
        assert(mapping.hasValue() &&
               "unexpected deletion of function argument");
        for (size_t j = 0; j < mapping->size; ++j)
          newArgAttrs[mapping->inputNo + j] = argAttrDicts[i];
      }
      attributes.push_back(
          rewriter.getNamedAttr(FunctionOpInterface::getArgDictAttrName(),
                                rewriter.getArrayAttr(newArgAttrs)));
    }
    for (const auto &pair : llvm::enumerate(attributes)) {
      if (pair.value().getName() == "llvm.linkage") {
        attributes.erase(attributes.begin() + pair.index());
        break;
      }
    }

    // Create an LLVM function, use external linkage by default until MLIR
    // functions have linkage.
    LLVM::Linkage linkage = LLVM::Linkage::External;
    if (funcOp->hasAttr("llvm.linkage")) {
      auto attr =
          funcOp->getAttr("llvm.linkage").dyn_cast<mlir::LLVM::LinkageAttr>();
      if (!attr) {
        funcOp->emitError()
            << "Contains llvm.linkage attribute not of type LLVM::LinkageAttr";
        return nullptr;
      }
      linkage = attr.getLinkage();
    }
    auto newFuncOp = rewriter.create<LLVM::LLVMFuncOp>(
        funcOp.getLoc(), funcOp.getName(), llvmType, linkage,
        /*dsoLocal*/ false, attributes);
    rewriter.inlineRegionBefore(funcOp.getBody(), newFuncOp.getBody(),
                                newFuncOp.end());

    if (failed(rewriter.convertRegionTypes(&newFuncOp.getBody(), *typeConverter,
                                           &result)))
      return nullptr;

    return newFuncOp;
  }
};

/// FuncOp legalization pattern that converts MemRef arguments to pointers to
/// MemRef descriptors (LLVM struct data types) containing all the MemRef type
/// information.
static constexpr StringRef kEmitIfaceAttrName = "llvm.emit_c_interface";
struct FuncOpConversion : public FuncOpConversionBase {
  FuncOpConversion(LLVMTypeConverter &converter, int numWarps)
      : FuncOpConversionBase(converter), NumWarps(numWarps) {}

  LogicalResult
  matchAndRewrite(FuncOp funcOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto newFuncOp = convertFuncOpToLLVMFuncOp(funcOp, rewriter);
    if (!newFuncOp)
      return failure();

    auto ctx = funcOp->getContext();
    auto i32 = IntegerType::get(ctx, 32);
    // Set an attribute for maxntidx, it could be used in latter LLVM codegen
    // for `nvvm.annotation` metadata.
    newFuncOp->setAttr(NVVMMetadataField::MaxNTid,
                       rewriter.getIntegerAttr(i32, 32 * NumWarps));

    rewriter.eraseOp(funcOp);
    return success();
  }

private:
  int NumWarps{0};
};

struct ReturnOpConversion : public ConvertOpToLLVMPattern<::mlir::ReturnOp> {
  using ConvertOpToLLVMPattern<ReturnOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(ReturnOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    unsigned numArguments = op.getNumOperands();

    // Currently, Triton kernel function always return nothing.
    // TODO(Superjomn) add support for non-inline device function
    if (numArguments > 0) {
      return rewriter.notifyMatchFailure(
          op, "Only kernel function with nothing returned is supported.");
    }

    rewriter.replaceOpWithNewOp<LLVM::ReturnOp>(op, TypeRange(), ValueRange(),
                                                op->getAttrs());
    return success();
  }
};

// Extract numWarps information from TritonGPU module, return 0 if failed.
// This is a naive implementation, it assumes that all the blocked layout should
// have the same numWarps setting in a module, it just find a blocked layout
// encoding and return the warpsPerCTA field.
int extractNumWarps(mlir::ModuleOp module) {
  int numWarps{};
  if (module->hasAttr(AttrNumWarpsName))
    numWarps = module->getAttr(AttrNumWarpsName)
                   .dyn_cast<IntegerAttr>()
                   .getValue()
                   .getZExtValue();
  else
    llvm::report_fatal_error(
        "TritonGPU module should contain a triton_gpu.num-warps attribute");

  return numWarps;
}

template <typename T>
static SmallVector<T> getMultiDimIndex(T linear_index, ArrayRef<T> shape) {
  // sizes {a, b, c, d}  ->  acc_mul {b*c*d, c*d, d, 1}
  size_t rank = shape.size();
  T acc_mul = 1;
  for (size_t i = 1; i < rank; ++i) {
    acc_mul *= shape[i];
  }
  T linear_remain = linear_index;
  SmallVector<T> multidim_index(rank);
  for (size_t i = 0; i < rank; ++i) {
    multidim_index[i] = linear_remain / acc_mul;
    linear_remain = linear_remain % acc_mul;
    if (i != (rank - 1)) {
      acc_mul = acc_mul / shape[i + 1];
    }
  }
  return multidim_index;
}

template <typename T>
static T getLinearIndex(ArrayRef<T> multidim_index, ArrayRef<T> shape) {
  assert(multidim_index.size() == shape.size());
  // sizes {a, b, c, d}  ->  acc_mul {b*c*d, c*d, d, 1}
  size_t rank = shape.size();
  T acc_mul = 1;
  for (size_t i = 1; i < rank; ++i) {
    acc_mul *= shape[i];
  }
  T linear_index = 0;
  for (size_t i = 0; i < rank; ++i) {
    linear_index += multidim_index[i] * acc_mul;
    if (i != (rank - 1)) {
      acc_mul = acc_mul / shape[i + 1];
    }
  }
  return linear_index;
}

static unsigned getElemsPerThread(const TritonGPUBlockedEncodingAttr &layout,
                                  ArrayRef<int64_t> shape) {
  unsigned elems = 1;
  size_t rank = shape.size();
  assert(rank == layout.getThreadsPerWarp().size());
  for (size_t d = 0; d < rank; ++d) {
    elems *=
        shape[d] / (layout.getThreadsPerWarp()[d] * layout.getWarpsPerCTA()[d]);
  }
  return elems;
}

static Value createIndexAttrConstant(OpBuilder &builder, Location loc,
                                     Type resultType, int64_t value) {
  return builder.create<LLVM::ConstantOp>(
      loc, resultType, builder.getIntegerAttr(resultType, value));
}

template <typename SourceOp>
class ConvertTritonGPUOpToLLVMPattern
    : public ConvertOpToLLVMPattern<SourceOp> {
public:
  using OpAdaptor = typename SourceOp::Adaptor;

  explicit ConvertTritonGPUOpToLLVMPattern(LLVMTypeConverter &typeConverter,
                                           PatternBenefit benefit = 1)
      : ConvertOpToLLVMPattern<SourceOp>(typeConverter, benefit) {}

  SmallVector<Value, 4>
  getElementsFromStruct(Location loc, Value llvmStruct, unsigned elems,
                        ConversionPatternRewriter &rewriter) const {
    SmallVector<Value, 4> results(elems);
    for (unsigned i = 0; i < elems; ++i) {
      Type type =
          llvmStruct.getType().cast<LLVM::LLVMStructType>().getBody()[i];
      results[i] = rewriter.create<LLVM::ExtractValueOp>(
          loc, type, llvmStruct, rewriter.getI64ArrayAttr(i));
    }
    return results;
  }

  Value getStructFromElements(Location loc, ValueRange resultVals,
                              ConversionPatternRewriter &rewriter,
                              Type structType) const {
    Value llvmStruct = rewriter.create<LLVM::UndefOp>(loc, structType);
    for (auto v : llvm::enumerate(resultVals)) {
      llvmStruct = rewriter.create<LLVM::InsertValueOp>(
          loc, structType, llvmStruct, v.value(),
          rewriter.getI64ArrayAttr(v.index()));
    }
    return llvmStruct;
  }

  SmallVector<Value> delinearize(ConversionPatternRewriter &rewriter,
                                 Location loc, Value linear,
                                 ArrayRef<unsigned> shape,
                                 ArrayRef<unsigned> order) const {
    unsigned rank = shape.size();
    assert(rank == order.size());
    SmallVector<unsigned> reordered(rank);
    for (unsigned i = 0; i < rank; ++i) {
      reordered[i] = shape[order[i]];
    }
    return delinearize(rewriter, loc, linear, reordered);
  }

  SmallVector<Value> delinearize(ConversionPatternRewriter &rewriter,
                                 Location loc, Value linear,
                                 ArrayRef<unsigned> shape) const {
    unsigned rank = shape.size();
    assert(rank > 0);
    SmallVector<Value> multiDim(rank);
    if (rank == 1) {
      multiDim[0] = linear;
    } else {
      Value remained = linear;
      for (auto &&en : llvm::enumerate(llvm::reverse(shape.drop_front()))) {
        Value dimSize = createIndexAttrConstant(
            rewriter, loc, this->getTypeConverter()->getIndexType(),
            en.value());
        multiDim[rank - 1 - en.index()] =
            rewriter.create<LLVM::URemOp>(loc, remained, dimSize);
        remained = rewriter.create<LLVM::UDivOp>(loc, remained, dimSize);
      }
      multiDim[0] = remained;
    }
    return multiDim;
  }

  // Emit indices calculation within each ConversionPattern
  // TODO: [goostavz] Double confirm the redundant indices calculations will
  //       be eliminated in the consequent MLIR/LLVM optimization
  SmallVector<SmallVector<Value>> emitIndicesForBlockedLayout(
      Location loc, ConversionPatternRewriter &b,
      const TritonGPUBlockedEncodingAttr &blocked_layout,
      ArrayRef<int64_t> shape) const {
    auto llvmIndexTy = this->getTypeConverter()->getIndexType();
    auto cast = b.create<UnrealizedConversionCastOp>(
        loc, TypeRange{llvmIndexTy},
        ValueRange{b.create<::mlir::gpu::ThreadIdOp>(
            loc, b.getIndexType(), ::mlir::gpu::Dimension::x)});
    Value threadId = cast.getResult(0);
    Value warpSize = createIndexAttrConstant(b, loc, llvmIndexTy, 32);
    Value laneId = b.create<LLVM::URemOp>(loc, threadId, warpSize);
    Value warpId = b.create<LLVM::UDivOp>(loc, threadId, warpSize);
    auto sizePerThread = blocked_layout.getSizePerThread();
    auto threadsPerWarp = blocked_layout.getThreadsPerWarp();
    auto warpsPerCTA = blocked_layout.getWarpsPerCTA();
    auto order = blocked_layout.getOrder();
    unsigned rank = shape.size();
    SmallVector<Value, 4> threadIds(rank);

    // step 1, delinearize threadId to get the base index
    SmallVector<Value> multiDimWarpId =
        delinearize(b, loc, warpId, warpsPerCTA, order);
    SmallVector<Value> multiDimThreadId =
        delinearize(b, loc, laneId, threadsPerWarp, order);
    SmallVector<Value> multiDimBase(rank);
    for (unsigned k = 0; k < rank; ++k) {
      // multiDimBase[k] = (multiDimThreadId[k] + multiDimWarpId[k] *
      // threadsPerWarp[k]) *
      //                   sizePerThread[k];
      Value threadsPerWarpK =
          createIndexAttrConstant(b, loc, llvmIndexTy, threadsPerWarp[k]);
      Value sizePerThreadK =
          createIndexAttrConstant(b, loc, llvmIndexTy, sizePerThread[k]);
      multiDimBase[k] = b.create<LLVM::MulOp>(
          loc, sizePerThreadK,
          b.create<LLVM::AddOp>(
              loc, multiDimThreadId[k],
              b.create<LLVM::MulOp>(loc, multiDimWarpId[k], threadsPerWarpK)));
    }

    // step 2, get offset of each element
    unsigned elemsPerThread = 1;
    SmallVector<SmallVector<unsigned>> offset(rank);
    SmallVector<unsigned> multiDimElemsPerThread(rank);
    for (unsigned k = 0; k < rank; ++k) {
      multiDimElemsPerThread[k] = shape[k] / threadsPerWarp[k] / warpsPerCTA[k];
      elemsPerThread *= multiDimElemsPerThread[k];
      for (unsigned blockOffset = 0;
           blockOffset <
           shape[k] / (sizePerThread[k] * threadsPerWarp[k] * warpsPerCTA[k]);
           ++blockOffset) {
        for (unsigned warpOffset = 0; warpOffset < warpsPerCTA[k];
             ++warpOffset) {
          for (unsigned threadOffset = 0; threadOffset < threadsPerWarp[k];
               ++threadOffset) {
            for (unsigned elemOffset = 0; elemOffset < sizePerThread[k];
                 ++elemOffset) {
              offset[k].push_back(blockOffset * sizePerThread[k] *
                                      threadsPerWarp[k] * warpsPerCTA[k] +
                                  warpOffset * sizePerThread[k] *
                                      threadsPerWarp[k] +
                                  threadOffset * sizePerThread[k] + elemOffset);
            }
          }
        }
      }
    }

    // step 3, add offset to base, and reorder the sequence of indices,
    //         to guarantee that elems in a same sizePerThread are adjacent in
    //         order
    SmallVector<SmallVector<Value>> multiDimIdx(elemsPerThread);
    unsigned accumSizePerThread =
        std::accumulate(sizePerThread.begin(), sizePerThread.end(), 1,
                        std::multiplies<unsigned>());
    SmallVector<unsigned> threadsPerDim(rank);
    for (unsigned k = 0; k < rank; ++k) {
      threadsPerDim[k] = shape[k] / sizePerThread[k];
    }
    for (unsigned n = 0; n < elemsPerThread; ++n) {
      unsigned linearNanoTileId = n / accumSizePerThread;
      unsigned linearElemsInNanoTileId = n % accumSizePerThread;
      SmallVector<unsigned> multiDimNanoTileId =
          getMultiDimIndex<unsigned>(linearNanoTileId, threadsPerDim);
      SmallVector<unsigned> multiElemsInNanoTileId =
          getMultiDimIndex<unsigned>(linearElemsInNanoTileId, sizePerThread);
      multiDimIdx[n].resize(rank);
      for (unsigned k = 0; k < rank; ++k) {
        unsigned reorderedMultiDimId =
            multiDimNanoTileId[k] *
                (sizePerThread[k] * threadsPerWarp[k] * warpsPerCTA[k]) +
            multiElemsInNanoTileId[k];
        multiDimIdx[n][k] = b.create<LLVM::AddOp>(
            loc, multiDimBase[k],
            createIndexAttrConstant(b, loc, llvmIndexTy,
                                    offset[k][reorderedMultiDimId]));
      }
    }

    return multiDimIdx;
  }
};

struct BroadcastOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::BroadcastOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::BroadcastOp>::ConvertTritonGPUOpToLLVMPattern;

  // Following the order of indices in the legacy code, a broadcast of:
  //   [s(0), s(1) ... s(k-1),    1, s(k+1), s(k+2) ... s(n-1)]
  // =>
  //   [s(0), s(1) ... s(k-1), s(k), s(k+1), s(k+2) ... s(n-1)]
  //
  // logically maps to a broadcast within a thread's scope:
  //   [cta(0)..cta(k-1),     1,cta(k+1)..cta(n-1),spt(0)..spt(k-1),
  //   1,spt(k+1)..spt(n-1)]
  // =>
  //   [cta(0)..cta(k-1),cta(k),cta(k+1)..cta(n-1),spt(0)..spt(k-1),spt(k),spt(k+1)..spt(n-1)]
  //
  // regardless of the order of the layout
  //
  LogicalResult
  matchAndRewrite(triton::BroadcastOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    Value src = adaptor.src();
    Value result = op.result();
    auto srcTy = op.src().getType().cast<RankedTensorType>();
    auto resultTy = result.getType().cast<RankedTensorType>();
    auto srcLayout =
        srcTy.getEncoding().dyn_cast<TritonGPUBlockedEncodingAttr>();
    auto resultLayout =
        resultTy.getEncoding().dyn_cast<TritonGPUBlockedEncodingAttr>();
    assert(srcLayout && (srcLayout == resultLayout) &&
           "Unexpected layout of BroadcastOp");
    auto srcShape = srcTy.getShape();
    auto resultShape = resultTy.getShape();
    unsigned rank = srcTy.getRank();
    // TODO: [goostavz] double confirm the op semantics with Phil
    assert(rank == resultTy.getRank());

    SmallVector<int64_t, 4> srcLogicalShape(2 * rank);
    SmallVector<int64_t, 4> resultLogicalShape(2 * rank);
    SmallVector<unsigned, 2> broadcastDims;
    SmallVector<int64_t, 2> broadcastSizes;
    int64_t duplicates = 1;
    for (unsigned d = 0; d < rank; ++d) {
      int64_t numCtas = resultShape[d] / (resultLayout.getSizePerThread()[d] *
                                          resultLayout.getThreadsPerWarp()[d] *
                                          resultLayout.getWarpsPerCTA()[d]);
      if (srcShape[d] != resultShape[d]) {
        assert(srcShape[d] == 1);
        broadcastDims.push_back(d);
        broadcastSizes.push_back(resultShape[d]);
        srcLogicalShape[d] = 1;
        srcLogicalShape[d + rank] = 1;
        duplicates *= resultShape[d];
      } else {
        srcLogicalShape[d] = numCtas;
        srcLogicalShape[d + rank] = resultLayout.getSizePerThread()[d];
      }
      resultLogicalShape[d] = numCtas;
      resultLogicalShape[d + rank] = resultLayout.getSizePerThread()[d];
    }
    unsigned srcElems = getElemsPerThread(srcLayout, srcShape);
    auto elemTy = resultTy.getElementType();
    auto srcVals = getElementsFromStruct(loc, src, srcElems, rewriter);
    unsigned resultElems = getElemsPerThread(resultLayout, resultShape);
    SmallVector<Value> resultVals(resultElems);
    for (unsigned i = 0; i < srcElems; ++i) {
      auto srcMultiDim = getMultiDimIndex<int64_t>(i, srcLogicalShape);
      auto resultMultiDim = srcMultiDim;
      for (int64_t j = 0; j < duplicates; ++j) {
        auto bcastMultiDim = getMultiDimIndex<int64_t>(j, broadcastSizes);
        for (auto bcastDim : llvm::enumerate(broadcastDims)) {
          resultMultiDim[bcastDim.value()] = bcastMultiDim[bcastDim.index()];
        }
        auto resultLinearIndex =
            getLinearIndex<int64_t>(resultMultiDim, resultLogicalShape);
        resultVals[resultLinearIndex] = srcVals[i];
      }
    }
    auto llvmStructTy = getTypeConverter()->convertType(resultTy);
    Value resultStruct =
        getStructFromElements(loc, resultVals, rewriter, llvmStructTy);
    rewriter.replaceOp(op, {resultStruct});
    return success();
  }
};

struct ViewOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::ViewOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::ViewOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::ViewOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // We cannot directly
    //   rewriter.replaceOp(op, adaptor.src());
    // due to MLIR's restrictions
    Location loc = op->getLoc();
    auto resultTy = op.getType().cast<RankedTensorType>();
    auto resultLayout =
        resultTy.getEncoding().dyn_cast<TritonGPUBlockedEncodingAttr>();
    auto resultShape = resultTy.getShape();
    unsigned elems = getElemsPerThread(resultLayout, resultShape);
    Type elemTy =
        this->getTypeConverter()->convertType(resultTy.getElementType());
    SmallVector<Type> types(elems, elemTy);
    Type structTy = LLVM::LLVMStructType::getLiteral(getContext(), types);
    auto vals = getElementsFromStruct(loc, adaptor.src(), elems, rewriter);
    Value view = getStructFromElements(loc, vals, rewriter, structTy);
    rewriter.replaceOp(op, view);
    return success();
  }
};

struct MakeRangeOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::MakeRangeOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::MakeRangeOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::MakeRangeOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    auto rankedTy = op.result().getType().dyn_cast<RankedTensorType>();
    auto shape = rankedTy.getShape();
    auto blocked_layout =
        rankedTy.getEncoding().dyn_cast<TritonGPUBlockedEncodingAttr>();
    auto elemTy = rankedTy.getElementType();
    assert(elemTy.isInteger(32));
    Value start = createIndexAttrConstant(rewriter, loc, elemTy, op.start());
    auto idxs =
        emitIndicesForBlockedLayout(loc, rewriter, blocked_layout, shape);
    unsigned elems = idxs.size();
    SmallVector<Value> retVals(elems);
    for (auto multiDim : llvm::enumerate(idxs)) {
      assert(multiDim.value().size() == 1);
      retVals[multiDim.index()] =
          rewriter.create<LLVM::AddOp>(loc, multiDim.value()[0], start);
    }
    SmallVector<Type> types(elems, elemTy);
    Type structTy = LLVM::LLVMStructType::getLiteral(getContext(), types);
    Value result = getStructFromElements(loc, retVals, rewriter, structTy);
    rewriter.replaceOp(op, result);
    return success();
  }
};

struct LoadOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::LoadOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::LoadOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::LoadOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    Value ptr = adaptor.ptr();
    Value mask = adaptor.mask();
    Value other = adaptor.other();
    auto resultTy = op.result().getType().cast<RankedTensorType>();
    auto blockedLayout =
        resultTy.getEncoding().dyn_cast<TritonGPUBlockedEncodingAttr>();
    auto shape = resultTy.getShape();

    // TODO: Handle AxisInfo
    //    vecWidth = std::min(nts, aln)
    // TODO: special processing for mma_first_row in legacy codes
    assert(blockedLayout && "LoadOp only accepts blocked_layout");
    unsigned vecWidth =
        blockedLayout.getSizePerThread()[blockedLayout.getOrder()[0]];

    auto elemTy = resultTy.getElementType();
    unsigned numElems = getElemsPerThread(blockedLayout, shape);
    auto ptrVals = getElementsFromStruct(loc, ptr, numElems, rewriter);
    auto maskVals = getElementsFromStruct(loc, mask, numElems, rewriter);
    auto otherVals = getElementsFromStruct(loc, other, numElems, rewriter);
    unsigned nbits = elemTy.isa<FloatType>()
                         ? elemTy.cast<FloatType>().getWidth()
                         : elemTy.cast<IntegerType>().getWidth();
    // unsigned dtsize = nbits / 8;
    int max_word_width = std::max<int>(32, nbits);
    int tot_width = nbits * vecWidth;
    int width = std::min(tot_width, max_word_width);
    int n_words = std::max(1, tot_width / width);
    // TODO: currently disable until supported in `store`
    bool has_l2_evict_policy = false;

    // TODO: (goostavz) handle when other is const but not splat, which
    //       should be rarely seen
    bool otherIsSplatConstInt = false;
    DenseElementsAttr constAttr;
    int64_t splatVal = 0;
    if (elemTy.isa<IntegerType>() &&
        matchPattern(op.other(), m_Constant(&constAttr)) &&
        constAttr.isSplat()) {
      otherIsSplatConstInt = true;
      splatVal = constAttr.getSplatValue<APInt>().getSExtValue();
    }

    SmallVector<Value> loadedVals;
    for (size_t i = 0; i < numElems; i += vecWidth) {
      Value ptr = ptrVals[i];
      // TODO: Handle the optimization if ptr is from GEP and the idx is
      // constant
      //       This should be a canonicalization pattern in LLVM Dialect
      unsigned in_off = 0;
      Value pred = maskVals[i];

      // ---
      // create inline asm string
      // ---
      // TODO: (Superjomn) refactor with AsmInstr abstraction
      std::ostringstream asmOss;
      asmOss << "@$" << n_words; // predicate
      asmOss << " ld";
      if (op.isVolatile()) {
        asmOss << ".volatile";
      }
      asmOss << ".global";
      if (op.cache() == triton::CacheModifier::CA)
        asmOss << ".ca";
      if (op.cache() == triton::CacheModifier::CG)
        asmOss << ".cg";
      if (op.evict() == triton::EvictionPolicy::EVICT_FIRST)
        asmOss << ".L1::evict_first";
      if (op.evict() == triton::EvictionPolicy::EVICT_LAST)
        asmOss << ".L1::evict_last";
      if (has_l2_evict_policy)
        asmOss << ".L2::cache_hint";
      if (n_words > 1)
        asmOss << ".v" << n_words; // vector width
      asmOss << ".b" << width;     // word size
      asmOss << " {";
      for (int i = 0; i < n_words; i++) { // return values
        if (i > 0)
          asmOss << ",";
        asmOss << "$" << i;
      }
      asmOss << "}";
      asmOss << ", [ $" << n_words + 1; // load
      asmOss << " + " << in_off << "]"; // constant offset
      if (has_l2_evict_policy)
        asmOss << ", $" << n_words + 2;
      asmOss << ";";
      SmallVector<Value> others;
      for (size_t ii = 0; ii < n_words; ii++) {
        size_t size = width / nbits;
        auto vecTy = LLVM::getFixedVectorType(elemTy, size);
        Value v = rewriter.create<LLVM::UndefOp>(loc, vecTy);
        for (size_t s = 0; s < size; s++) {
          Value falseVal = otherVals[i + ii * size + s];
          Value sVal = createIndexAttrConstant(
              rewriter, loc, this->getTypeConverter()->getIndexType(), s);
          v = rewriter.create<LLVM::InsertElementOp>(loc, vecTy, v, falseVal,
                                                     sVal);
        }
        v = rewriter.create<LLVM::BitcastOp>(
            loc, IntegerType::get(getContext(), width), v);
        asmOss << "\n        ";
        asmOss << "@!$" << n_words << " mov.u" << width;
        asmOss << " $" << ii << ", ";
        std::ios_base::fmtflags flags(asmOss.flags());
        if (otherIsSplatConstInt)
          asmOss << "0x" << std::hex << splatVal;
        else {
          asmOss << "$" << n_words + has_l2_evict_policy + 2 + ii;
          others.push_back(v);
        }
        asmOss.flags(flags);
        asmOss << ";";
      }
      // ---
      // create inline ASM signature
      // ---
      SmallVector<Type> retTys(n_words, IntegerType::get(getContext(), width));
      Type retTy = retTys.size() > 1
                       ? LLVM::LLVMStructType::getLiteral(getContext(), retTys)
                       : retTys[0];
      // ---
      // create inline ASM constraints
      // ---
      std::string asmCstrt;
      for (int ii = 0; ii < n_words; ii++) {
        if (ii > 0)
          asmCstrt += ",";
        asmCstrt += (width == 64) ? "=l" : ((width == 32) ? "=r" : "=c");
      }
      asmCstrt += ",b,l";
      for (size_t ii = 0; ii < others.size(); ii++) {
        asmCstrt += ",";
        asmCstrt += (width == 64) ? "l" : ((width == 32) ? "r" : "c");
      }
      if (has_l2_evict_policy) {
        asmCstrt += ",l";
      }
      // ---
      // finally call inline ASM
      // ---
      SmallVector<Value> args = {pred, ptr};
      auto asmDialectAttr = LLVM::AsmDialectAttr::get(rewriter.getContext(),
                                                      LLVM::AsmDialect::AD_ATT);
      auto inlineAsmOp = rewriter.create<LLVM::InlineAsmOp>(
          loc, retTy, /*operands=*/args, /*asm_string=*/asmOss.str(),
          /*constraints=*/asmCstrt, /*has_side_effects=*/true,
          /*is_align_stack=*/false, /*asm_dialect=*/asmDialectAttr,
          /*operand_attrs=*/ArrayAttr());
      Value ret = inlineAsmOp.getResult(0);
      // ---
      // extract and store return values
      // ---
      SmallVector<Value> rets;
      for (unsigned int ii = 0; ii < n_words; ii++) {
        Value curr = nullptr;
        if (retTy.isa<LLVM::LLVMStructType>()) {
          curr = rewriter.create<LLVM::ExtractValueOp>(
              loc, IntegerType::get(getContext(), width), ret,
              rewriter.getI64ArrayAttr(ii));
        } else {
          curr = ret;
        }
        curr = rewriter.create<LLVM::BitcastOp>(
            loc, LLVM::getFixedVectorType(elemTy, width / nbits), curr);
        rets.push_back(curr);
      }
      int tmp = (width / nbits);
      for (size_t ii = 0; ii < vecWidth; ii++) {
        Value vecIdx = createIndexAttrConstant(
            rewriter, loc, this->getTypeConverter()->getIndexType(), ii % tmp);
        Value loaded = rewriter.create<LLVM::ExtractElementOp>(
            loc, elemTy, rets[ii / tmp], vecIdx);
        loadedVals.push_back(loaded);
      }
    }
    Type llvmResultStructTy = getTypeConverter()->convertType(resultTy);
    Value resultStruct =
        getStructFromElements(loc, loadedVals, rewriter, llvmResultStructTy);
    rewriter.replaceOp(op, {resultStruct});
    return success();
  }
};

class TritonGPUToLLVMTypeConverter : public LLVMTypeConverter {
public:
  using TypeConverter::convertType;

  TritonGPUToLLVMTypeConverter(MLIRContext *ctx, LowerToLLVMOptions &option,
                               const DataLayoutAnalysis *analysis = nullptr)
      : LLVMTypeConverter(ctx, option, analysis) {
    addConversion([&](triton::PointerType type) -> llvm::Optional<Type> {
      return convertTritonPointerType(type);
    });
    addConversion([&](RankedTensorType type) -> llvm::Optional<Type> {
      return convertTritonTensorType(type);
    });
  }

  Type convertTritonPointerType(triton::PointerType type) {
    return LLVM::LLVMPointerType::get(type.getPointeeType(),
                                      type.getAddressSpace());
  }

  llvm::Optional<Type> convertTritonTensorType(RankedTensorType type) {
    Attribute layout = type.getEncoding();
    if (auto blocked_layout = layout.dyn_cast<TritonGPUBlockedEncodingAttr>()) {
      unsigned numElementsPerThread =
          getElemsPerThread(blocked_layout, type.getShape());
      SmallVector<Type, 4> types(numElementsPerThread,
                                 convertType(type.getElementType()));
      return LLVM::LLVMStructType::getLiteral(&getContext(), types);
    } else if (auto mma_layout = layout.dyn_cast<TritonGPUMmaEncodingAttr>()) {
      // TODO: Not implemented
      return llvm::None;
    } else if (auto shared_layout =
                   layout.dyn_cast<TritonGPUSharedEncodingAttr>()) {
      // TODO: Not implemented
      return llvm::None;
    }
    return llvm::None;
  }
};

void populateTritonToLLVMPatterns(mlir::LLVMTypeConverter &typeConverter,
                                  RewritePatternSet &patterns, int numWarps) {
  patterns.add<BroadcastOpConversion>(typeConverter);
  patterns.add<FuncOpConversion>(typeConverter, numWarps);
  patterns.add<LoadOpConversion>(typeConverter);
  patterns.add<MakeRangeOpConversion>(typeConverter);
  patterns.add<ReturnOpConversion>(typeConverter);
  patterns.add<ViewOpConversion>(typeConverter);
}

class ConvertTritonGPUToLLVM
    : public ConvertTritonGPUToLLVMBase<ConvertTritonGPUToLLVM> {
public:
  ConvertTritonGPUToLLVM() = default;

  void runOnOperation() override {
    MLIRContext *context = &getContext();
    ModuleOp mod = getOperation();

    mlir::LowerToLLVMOptions option(context);
    // TODO: need confirm
    option.overrideIndexBitwidth(32);
    TritonGPUToLLVMTypeConverter typeConverter(context, option);
    TritonLLVMConversionTarget target(*context, typeConverter);

    RewritePatternSet patterns(context);
    // TODO: (goostavz) Temporarily disable this, since the lowering of
    //       arithmetic ops in tensor format is not complete yet.
    // Add arith's patterns to help convert scalar expression to LLVM.
    // mlir::arith::populateArithmeticToLLVMConversionPatterns(typeConverter,
    //                                                         patterns);

    int numWarps = extractNumWarps(mod);

    populateTritonToLLVMPatterns(typeConverter, patterns, numWarps);
    mlir::populateGpuToNVVMConversionPatterns(typeConverter, patterns);

    if (failed(applyPartialConversion(mod, target, std::move(patterns))))
      return signalPassFailure();
  }
};

} // namespace

namespace mlir {

TritonLLVMConversionTarget::TritonLLVMConversionTarget(
    MLIRContext &ctx, mlir::LLVMTypeConverter &typeConverter)
    : ConversionTarget(ctx), typeConverter(typeConverter) {
  addLegalDialect<LLVM::LLVMDialect>();
  addLegalDialect<NVVM::NVVMDialect>();
  // addIllegalDialect<triton::TritonDialect>();
  addIllegalDialect<mlir::gpu::GPUDialect>();
  addLegalOp<mlir::UnrealizedConversionCastOp>();
}

namespace triton {

std::unique_ptr<OperationPass<ModuleOp>> createConvertTritonGPUToLLVMPass() {
  return std::make_unique<::ConvertTritonGPUToLLVM>();
}

} // namespace triton
} // namespace mlir
