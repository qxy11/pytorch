#include <torch/csrc/jit/runtime/static/ops.h>

#include <ATen/CPUFunctions.h>
#include <ATen/InferSize.h>
#include <ATen/NativeFunctions.h>
#include <ATen/ScalarOps.h>
#include <ATen/TensorUtils.h>
#include <ATen/native/EmbeddingBag.h>
#include <ATen/native/IndexingUtils.h>
#include <ATen/native/Resize.h>
#include <ATen/native/TensorAdvancedIndexing.h>
#include <ATen/native/layer_norm.h>
#include <ATen/native/quantized/cpu/qembeddingbag.h>
#include <c10/util/irange.h>
#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/runtime/vararg_functions.h>
#include <torch/csrc/jit/tensorexpr/ir.h>
#include <torch/csrc/jit/tensorexpr/ir_simplifier.h>
#include <torch/csrc/jit/tensorexpr/llvm_codegen.h>
#include <torch/csrc/jit/tensorexpr/loopnest.h>

namespace at {
namespace native {
// copy version of view ops
at::Tensor& reshape_copy_out(
    at::Tensor& out,
    const at::Tensor& self,
    const std::vector<int64_t>& proposed_shape,
    bool infer_size) {
  auto shape = infer_size ? at::infer_size(proposed_shape, self.numel())
                          : proposed_shape;
  at::native::resize_(out, shape, c10::nullopt);

  auto self_contig = self.expect_contiguous();

  size_t nbytes = self.nbytes();
  if (nbytes == 0) {
    return out;
  }

  const void* self_data = self_contig->data_ptr();
  void* out_data = out.data_ptr();
  memcpy(out_data, self_data, nbytes);

  return out;
}

at::Tensor& flatten_copy_out(
    at::Tensor& out,
    const at::Tensor& self,
    int64_t start_dim,
    int64_t end_dim) {
  start_dim =
      start_dim < 0 ? c10::maybe_wrap_dim(start_dim, self.dim()) : start_dim;
  end_dim = end_dim < 0 ? c10::maybe_wrap_dim(end_dim, self.dim()) : end_dim;
  TORCH_CHECK(
      start_dim <= end_dim,
      "flatten() has invalid args: start_dim cannot come after end_dim");

  if (self.dim() == 0) {
    return reshape_copy_out(out, self, {1}, false);
  }

  if (start_dim == end_dim) {
    auto shape = self.sizes().vec();
    return reshape_copy_out(out, self, shape, false);
  }

  // We don't want to infer_size on the entire shape, because that can give us
  // an extra degree of freedom we don't want; for example, consider shape [0,
  // 1, 3, 0], with start_dim=1, end_dim=2. It's clear we want result shape [0,
  // 3, 0] but passing [0, -1, 0] to infer_size means the -1 can take on any
  // value and satisfy the constraints.
  auto iter = self.sizes().data();
  auto slice_numel = std::accumulate(
      iter + start_dim,
      iter + end_dim + 1,
      static_cast<int64_t>(1),
      // NOLINTNEXTLINE(modernize-use-transparent-functors)
      std::multiplies<int64_t>());

  std::vector<int64_t> shape;
  shape.reserve(self.dim() - end_dim + start_dim);
  for (const auto i : c10::irange(start_dim)) {
    shape.push_back(self.sizes()[i]);
  }
  shape.push_back(slice_numel);
  for (int64_t i = end_dim + 1; i < self.dim(); i++) {
    shape.push_back(self.sizes()[i]);
  }
  return reshape_copy_out(out, self, shape, false);
}

at::Tensor& to_copy_out(Tensor& out, const Tensor& self, bool non_blocking) {
  if (!out.options().memory_format_opt().has_value()) {
    at::native::resize_impl_cpu_(
        out.unsafeGetTensorImpl(), self.sizes(), self.strides());
    at::native::copy_(out, self, non_blocking);
    return out;
  }
  at::native::resize_(out, self.sizes(), c10::nullopt);
  at::native::copy_(out, self, non_blocking);
  return out;
}
} // namespace native
} // namespace at

namespace torch {
namespace jit {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
C10_DEFINE_REGISTRY(SROperatorRegistry, SROperatorFunctor);

bool opIsRegistered(const c10::Symbol& op_name) {
  const std::string name(op_name.toQualString());
  return SROperatorRegistry()->Has(name);
}

bool canRunOutOfPlace(Node* n) {
  auto op_name = std::string(n->kind().toQualString());
  return SROperatorRegistry()->Has(op_name);
}

// Keep function canReuseInputsOutputs because the name canReuseInputsOutputs is
// more informative where it's used
bool canReuseInputsOutputs(Node* n) {
  return canRunOutOfPlace(n);
}

// TODO: expand to include all view producing ops, mostly in
// https://github.com/pytorch/pytorch/blob/master/aten/src/ATen/native/TensorShape.cpp
bool canRunNatively(Node* n) {
  // In alphabetical order
  const static std::unordered_set<std::string> native_nodes{
      "aten::flatten",
      "aten::reshape",
      "aten::slice",
      "aten::transpose",
      "aten::to",
      "prim::ListConstruct",
      "prim::ListUnpack",
      "prim::TupleConstruct",
      "prim::DictConstruct",
      "aten::__getitem__"};
  auto str = std::string(n->kind().toQualString());
  if (!native_nodes.count(str)) {
    return false;
  }
  if (str == "aten::to") {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
    return n->inputs().size() == 5;
  }
  return true;
}

// returns true if the producers of the inputs
// to this operations are out of place.
// This means the IValues will not change run to run
bool inputsCanRunOutOfPlace(Node* n) {
  for (auto* input : n->inputs()) {
    if (!canRunOutOfPlace(input->node())) {
      return false;
    }
  }
  return true;
}

bool isOptimizableContainerType(Node* n) {
  const auto& type = n->output()->type();
  if (type->kind() == TypeKind::ListType) {
    const auto& list_type = type->expectRef<ListType>();
    bool is_tensor_list =
        list_type.getElementType()->kind() == TypeKind::TensorType;
    return is_tensor_list && inputsCanRunOutOfPlace(n);
  } else if (type->kind() == TypeKind::TupleType) {
    const auto& tuple_type = type->expectRef<TupleType>();
    auto types = tuple_type.containedTypes();
    const auto& iter =
        std::find_if(types.begin(), types.end(), [](const TypePtr& elem) {
          return elem->kind() == TypeKind::TensorType;
        });
    bool is_tensor_tuple = iter != types.end();
    return is_tensor_tuple && inputsCanRunOutOfPlace(n);
  }
  return false;
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(
    prim::ListConstruct,
    prim_ListConstruct,
    [](Node* n) -> SROperator {
      const auto& type = n->output()->type()->expectRef<ListType>();
      bool can_optimize = isOptimizableContainerType(n);
      return [can_optimize, &type](ProcessedNode* p_node) {
        const auto& out_l = p_node->Output(0);
        if (!out_l.isNone() && can_optimize) {
          return;
        }
        const size_t size = p_node->inputs().size();
        c10::List<IValue> vals(type.getElementType());
        vals.reserve(size);
        for (size_t i = 0; i < size; i++) {
          vals.push_back(p_node->Input(i));
        }
        p_node->Output(0) = std::move(vals);
      };
    });

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(
    prim::TupleConstruct,
    prim_TupleConstruct,
    [](Node* n) -> SROperator {
      bool can_optimize = isOptimizableContainerType(n);
      return [can_optimize](ProcessedNode* p_node) {
        const auto& out_l = p_node->Output(0);
        if (!out_l.isNone() && can_optimize) {
          return;
        }
        // prepare inputs
        const size_t size = p_node->inputs().size();
        std::vector<IValue> vals;
        vals.reserve(size);
        for (size_t i = 0; i < size; i++) {
          vals.push_back(p_node->Input(i));
        }
        p_node->Output(0) = c10::ivalue::Tuple::create(std::move(vals));
      };
    });

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::mul, aten_mul, [](Node* n) -> SROperator {
  return [](ProcessedNode* p_node) {
    const auto& in0_t = p_node->Input(0).toTensor();
    const auto& in1_t = p_node->Input(1).toTensor();
    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(in0_t);
    }
    auto& out_t = p_node->Output(0).toTensor();
    fastResizeToZero(out_t);
    at::cpu::mul_out(out_t, in0_t, in1_t);
  };
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::addmm, aten_addmm, [](Node* n) -> SROperator {
  return [](ProcessedNode* p_node) {
    const auto& in0_t = p_node->Input(0).toTensor();
    const auto& in1_t = p_node->Input(1).toTensor();
    const auto& in2_t = p_node->Input(2).toTensor();
    const auto in3_s = p_node->Input(3).toScalar();
    const auto in4_s = p_node->Input(4).toScalar();
    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(in0_t);
    }
    auto& out_t = p_node->Output(0).toTensor();
    fastResizeToZero(out_t);
    at::native::addmm_cpu_out(in0_t, in1_t, in2_t, in3_s, in4_s, out_t);
  };
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::clamp, aten_clamp, [](Node* n) -> SROperator {
  return [](ProcessedNode* p_node) {
    const auto& in0_t = p_node->Input(0).toTensor();
    const auto in1_s = p_node->Input(1).toOptional<at::Scalar>();
    const auto in2_s = p_node->Input(2).toOptional<at::Scalar>();
    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(in0_t);
    }
    auto& out_t = p_node->Output(0).toTensor();
    fastResizeToZero(out_t);
    at::native::clamp_out(in0_t, in1_s, in2_s, out_t);
  };
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::bmm, aten_bmm, [](Node* n) -> SROperator {
  return [](ProcessedNode* p_node) {
    const auto& in0_t = p_node->Input(0).toTensor();
    const auto& in1_t = p_node->Input(1).toTensor();
    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(in0_t);
    }
    auto& out_t = p_node->Output(0).toTensor();
    fastResizeToZero(out_t);
    at::native::bmm_out_cpu(in0_t, in1_t, out_t);
  };
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(
    aten::nan_to_num,
    aten_nan_to_num,
    [](Node* n) -> SROperator {
      return [](ProcessedNode* p_node) {
        const auto& in0_t = p_node->Input(0).toTensor();
        const auto in1_d = p_node->Input(1).toOptional<double>();
        const auto in2_d = p_node->Input(2).toOptional<double>();
        const auto in3_d = p_node->Input(3).toOptional<double>();
        if (p_node->Output(0).isNone()) {
          p_node->Output(0) = create_empty_from(in0_t);
        }
        auto& out_t = p_node->Output(0).toTensor();
        fastResizeToZero(out_t);
        at::native::nan_to_num_out(in0_t, in1_d, in2_d, in3_d, out_t);
      };
    });
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::cat, aten_cat, [](Node* n) -> SROperator {
  return [](ProcessedNode* p_node) {
    const auto in0_tl = p_node->Input(0).toTensorVector();
    const auto in1_i = p_node->Input(1).toInt();
    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(in0_tl[0]);
    }
    auto& out_t = p_node->Output(0).toTensor();
    fastResizeToZero(out_t);
    at::native::_cat_out_cpu(in0_tl, in1_i, out_t);
  };
});

// Split out into a function to appease MSVC's pre-processor
SROperator aten_stack(Node* n) {
  return [](ProcessedNode* p_node) {
    const auto inputs = p_node->Input(0).toTensorVector();
    const auto dim = p_node->Input(1).toInt();
    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(inputs[0]);
    }
    auto& out_t = p_node->Output(0).toTensor();
    fastResizeToZero(out_t);
    at::native::_stack_out_cpu(inputs, dim, out_t);
  };
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::stack, aten_stack, aten_stack);

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(
    aten::leaky_relu,
    aten_leaky_relu,
    [](Node* n) -> SROperator {
      return [](ProcessedNode* p_node) {
        const auto& in0_t = p_node->Input(0).toTensor();
        const auto in1_s = p_node->Input(1).toScalar();
        if (p_node->Output(0).isNone()) {
          p_node->Output(0) = create_empty_from(in0_t);
        }
        auto& out_t = p_node->Output(0).toTensor();
        at::native::leaky_relu_out(in0_t, in1_s, out_t);
      };
    });

namespace {

// Use the width of an AVX-512 vector by default; this happens to work OK for
// AVX2 as well. Some ops benefit from using multiple AVX ports, in which case
// they are vectorized by twice this constant.  An exception is logit, since it
// contains FP divide, which is single-ported.
static constexpr int kVectorWidth = 16;

#ifdef TORCH_ENABLE_LLVM

struct TEWrapper {
  tensorexpr::KernelArena ka;
  tensorexpr::KernelScope ks;
  std::unique_ptr<tensorexpr::LLVMCodeGen> cg;
  TEWrapper() = default;
  void update(std::unique_ptr<tensorexpr::LLVMCodeGen>&& cg_) {
    cg = std::move(cg_);
  }
  template <typename... Ts>
  void operator()(const Ts&... ts) {
    std::vector<tensorexpr::CodeGen::CallArg> args(
        {tensorexpr::CodeGen::CallArg(ts)...});
    cg->call(args);
  }

  inline bool supports(const at::Tensor& t) {
    return t.is_contiguous() && t.dtype().Match<float>();
  }
};

void optimizePointwise(
    tensorexpr::LoopNest* ln,
    tensorexpr::Tensor* target,
    int width) {
  using namespace torch::jit::tensorexpr;
  std::vector<For*> loops = ln->getLoopStmtsFor(target);
  For *outer, *inner, *tail;
  TORCH_CHECK(loops.size() > 0, "No loops created for pointwise op");
  ln->splitWithTail(loops[0], width, &outer, &inner, &tail);
  ln->vectorize(inner);
}

std::shared_ptr<TEWrapper> wrapTECompute(
    std::shared_ptr<TEWrapper> wrap,
    tensorexpr::Placeholder& in,
    tensorexpr::Tensor* out,
    tensorexpr::VarHandle& dim,
    int width = kVectorWidth) {
  using namespace torch::jit::tensorexpr;
  LoopNest ln({out});
  optimizePointwise(&ln, out, width);
  ln.prepareForCodegen();
  Stmt* s = ln.root_stmt();
  s = tensorexpr::IRSimplifier::simplify(s);
  std::vector<CodeGen::BufferArg> args;
  args.emplace_back(out);
  args.emplace_back(in);
  args.emplace_back(dim);
  auto cg = std::make_unique<LLVMCodeGen>(s, args);
  wrap->update(std::move(cg));
  return wrap;
};

#else

struct TEWrapper {
  TEWrapper() = default;
  template <typename... Ts>
  void operator()(const Ts&... ts) {
    DCHECK(0 && "Invalid call");
  }

  inline bool supports(const at::Tensor& t) {
    return false;
  }
};

std::shared_ptr<TEWrapper> wrapTECompute(
    std::shared_ptr<TEWrapper> wrap,
    tensorexpr::Placeholder& in,
    tensorexpr::Tensor* out,
    tensorexpr::VarHandle& dim,
    int width = kVectorWidth) {
  return wrap;
};

#endif

} // namespace

std::shared_ptr<TEWrapper> createLogit(c10::optional<float> clamp) {
  using namespace torch::jit::tensorexpr;
  auto wrap = std::make_shared<TEWrapper>();
  auto N = VarHandle("N", kInt);
  Placeholder A("A", kFloat, {N});
  tensorexpr::Tensor* B = Compute("B", {N}, [&](const VarHandle& i) {
    auto A_elem = [&]() {
      if (!clamp) {
        return A.load(i);
      } else {
        auto elem = A.load(i);
        auto min = FloatImm::make(*clamp);
        auto max = FloatImm::make(1.0f - *clamp);
        elem = CompareSelect::make(elem, min, min, elem, kLT);
        return CompareSelect::make(elem, max, max, elem, kGT);
      }
    }();
    return log_vml(A_elem / (FloatImm::make(1.0f) - A_elem));
  });
  return wrapTECompute(wrap, A, B, N);
}

std::shared_ptr<TEWrapper> createRelu() {
  using namespace torch::jit::tensorexpr;
  auto wrap = std::make_shared<TEWrapper>();
  auto N = VarHandle("N", kInt);
  Placeholder A("A", kFloat, {N});
  tensorexpr::Tensor* B = Compute("B", {N}, [&](const VarHandle& i) {
    auto zero = FloatImm::make(0.f);
    auto a = A.load(i);
    return ifThenElse(a < zero, zero, a);
  });
  return wrapTECompute(wrap, A, B, N);
}

std::shared_ptr<TEWrapper> createTanh() {
  using namespace torch::jit::tensorexpr;
  auto wrap = std::make_shared<TEWrapper>();
  auto N = VarHandle("N", kInt);
  Placeholder A("A", kFloat, {N});
  tensorexpr::Tensor* B = Compute("B", {N}, [&](const VarHandle& i) {
    auto a = A.load(i);
    return fast_tanh(a);
  });
  return wrapTECompute(wrap, A, B, N);
}

std::shared_ptr<TEWrapper> createSigmoid() {
  using namespace torch::jit::tensorexpr;
  auto wrap = std::make_shared<TEWrapper>();
  auto N = VarHandle("N", kInt);
  Placeholder A("A", kFloat, {N});
  Tensor* B =
      Compute("B", {N}, [&](const VarHandle& i) { return sigmoid(A.load(i)); });
  // NNC uses sleef for vectorizing sigmoid, which comes in an 8-wide flavor
  // (Sleef_expf8).
  constexpr int kSleefWidth = 8;
  return wrapTECompute(wrap, A, B, N, kSleefWidth);
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::relu, aten_relu, [](Node* n) -> SROperator {
  auto te = createRelu();
  return [te](ProcessedNode* p_node) {
    const auto& in0_t = p_node->Input(0).toTensor();
    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(in0_t);
    }
    auto& out_t = p_node->Output(0).toTensor();
    if (!te->supports(in0_t)) {
      fastResizeToZero(out_t);
      at::native::threshold_out(in0_t, 0, 0, out_t);
    } else {
      at::native::resize_(out_t, in0_t.sizes(), c10::nullopt);
      (*te)(out_t.data_ptr<float>(), in0_t.data_ptr<float>(), in0_t.numel());
    }
  };
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::tanh, aten_tanh, [](Node* n) -> SROperator {
  auto te = createTanh();
  return [te](ProcessedNode* p_node) {
    const auto& in0_t = p_node->Input(0).toTensor();
    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(in0_t);
    }
    auto& out_t = p_node->Output(0).toTensor();
    if (!te->supports(in0_t)) {
      fastResizeToZero(out_t);
      at::cpu::tanh_out(out_t, in0_t);
    } else {
      at::native::resize_(out_t, in0_t.sizes(), c10::nullopt);
      (*te)(out_t.data_ptr<float>(), in0_t.data_ptr<float>(), in0_t.numel());
    }
  };
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(
    aten::sigmoid,
    aten_sigmoid,
    [](Node* n) -> SROperator {
      auto te = createSigmoid();
      return [te](ProcessedNode* p_node) {
        const auto& in0_t = p_node->Input(0).toTensor();
        if (p_node->Output(0).isNone()) {
          p_node->Output(0) = create_empty_from(in0_t);
        }
        auto& out_t = p_node->Output(0).toTensor();
        if (!te->supports(in0_t)) {
          fastResizeToZero(out_t);
          at::cpu::sigmoid_out(out_t, in0_t);
        } else {
          at::native::resize_(out_t, in0_t.sizes(), c10::nullopt);
          (*te)(
              out_t.data_ptr<float>(), in0_t.data_ptr<float>(), in0_t.numel());
        }
      };
    });

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::logit, aten_logit, [](Node* n) -> SROperator {
  c10::optional<float> clamp;
  if (n->inputs().size() > 1) {
    TORCH_CHECK(n->inputs().at(1)->node()->kind() == prim::Constant);
    clamp = toIValue(n->inputs().at(1))->toDouble();
  }
  auto te = createLogit(clamp);
  return [te](ProcessedNode* p_node) {
    const auto& in0_t = p_node->Input(0).toTensor();
    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(in0_t);
    }
    auto& out_t = p_node->Output(0).toTensor();
    if (!te->supports(in0_t)) {
      const auto& in0_t = p_node->Input(0).toTensor();
      const auto in1_d = p_node->Input(1).toOptional<double>();
      fastResizeToZero(out_t);
      at::native::logit_out(in0_t, in1_d, out_t);
    } else {
      at::native::resize_(out_t, in0_t.sizes(), c10::nullopt);
      (*te)(out_t.data_ptr<float>(), in0_t.data_ptr<float>(), in0_t.numel());
    }
  };
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::clone, aten_clone, [](Node* n) -> SROperator {
  return [](ProcessedNode* p_node) {
    const auto& in0_t = p_node->Input(0).toTensor();
    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(in0_t);
    }
    auto& out_t = p_node->Output(0).toTensor();
    at::native::resize_(out_t, in0_t.sizes(), c10::nullopt);
    at::native::copy_(out_t, in0_t, false);
  };
});
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(
    quantized::embedding_bag_byte_rowwise_offsets,
    quantized_embedding_bag_byte_rowwise_offsets,
    [](Node* n) -> SROperator {
      return [](ProcessedNode* p_node) {
        const auto& weight = p_node->Input(0).toTensor();
        const auto& indices = p_node->Input(1).toTensor();
        const auto offsets = p_node->Input(2).toOptional<at::Tensor>();
        const auto pruned_weights = p_node->Input(5).toBool();
        const auto per_sample_weights =
            p_node->Input(6).toOptional<at::Tensor>();
        const auto compressed_indices_mapping =
            p_node->Input(7).toOptional<at::Tensor>();
        const auto include_last_offset = p_node->Input(8).toBool();
        if (p_node->Output(0).isNone()) {
          p_node->Output(0) = create_empty_from(weight, at::kFloat);
        }
        auto& out_t = p_node->Output(0).toTensor();
        fastResizeToZero(out_t);
        return at::native::embedding_bag_byte_rowwise_offsets_out(
            out_t,
            weight,
            indices,
            offsets,
            false, // unused scale_grad_by_freq
            0, // unused mode
            pruned_weights,
            per_sample_weights,
            compressed_indices_mapping,
            include_last_offset);
      };
    });
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(
    quantized::embedding_bag_4bit_rowwise_offsets,
    embedding_bag_4bit_rowwise_offsets,
    [](Node* n) -> SROperator {
      return [](ProcessedNode* p_node) {
        const auto& weight = p_node->Input(0).toTensor();
        const auto& indices = p_node->Input(1).toTensor();
        const auto offsets = p_node->Input(2).toOptional<at::Tensor>();
        const auto pruned_weights = p_node->Input(5).toBool();
        const auto per_sample_weights =
            p_node->Input(6).toOptional<at::Tensor>();
        const auto compressed_indices_mapping =
            p_node->Input(7).toOptional<at::Tensor>();
        const auto include_last_offset = p_node->Input(8).toBool();
        if (p_node->Output(0).isNone()) {
          p_node->Output(0) = create_empty_from(weight, at::kFloat);
        }
        auto& out_t = p_node->Output(0).toTensor();
        fastResizeToZero(out_t);
        return at::native::embedding_bag_4bit_rowwise_offsets_out(
            out_t,
            weight,
            indices,
            offsets,
            false, // unused scale_grad_by_freq
            0, // unused mode
            pruned_weights,
            per_sample_weights,
            compressed_indices_mapping,
            include_last_offset);
      };
    });

// The out variant takes precedence over native
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(
    aten::narrow_copy,
    aten_narrow_copy,
    [](Node* n) -> SROperator {
      return [](ProcessedNode* p_node) {
        const auto& self = p_node->Input(0).toTensor(); // self
        const auto dim = p_node->Input(1).toInt(); // dim
        int64_t start = 0;
        if (p_node->Input(2).isScalar()) {
          start = p_node->Input(2).toInt();
        } else {
          auto& t = p_node->Input(2).toTensor();
          start = t.item<int64_t>();
        }
        auto length = p_node->Input(3).toInt(); // length

        if (p_node->Output(0).isNone()) {
          p_node->Output(0) = create_empty_from(self);
        }
        auto& output = p_node->Output(0).toTensor();
        fastResizeToZero(output);
        at::native::narrow_copy_dense_cpu_out(self, dim, start, length, output);
      };
    });
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::index, aten_index, [](Node* n) -> SROperator {
  return [](ProcessedNode* p_node) {
    const auto& in0_t = p_node->Input(0).toTensor();
    const auto in1_l =
        at::native::toListOfOptionalTensors(p_node->Input(1).toListRef());
    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(in0_t);
    }
    auto& out_t = p_node->Output(0).toTensor();
    fastResizeToZero(out_t);
    at::native::index_out(out_t, in0_t, in1_l);
  };
});
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::pow, aten_pow, [](Node* n) -> SROperator {
  return [](ProcessedNode* p_node) {
    if (p_node->Output(0).isNone()) {
      c10::ScalarType dtype;
      if (p_node->Input(0).isTensor()) {
        const auto& in0_t = p_node->Input(0).toTensor();
        if (p_node->Input(1).isTensor()) {
          dtype = at::native::result_type(in0_t, p_node->Input(1).toTensor());
          p_node->Output(0) = create_empty_from(in0_t, dtype);
        } else {
          dtype = at::native::result_type(in0_t, p_node->Input(1).toScalar());
          p_node->Output(0) = at::native::empty_like(
              in0_t,
              dtype,
              in0_t.options().layout_opt(),
              in0_t.options().device_opt(),
              in0_t.options().pinned_memory_opt(),
              at::MemoryFormat::Preserve);
        }
      } else {
        const auto& in1_t = p_node->Input(1).toTensor();
        dtype = at::native::result_type(p_node->Input(0).toScalar(), in1_t);
        p_node->Output(0) = at::native::empty_like(
            in1_t,
            dtype,
            in1_t.options().layout_opt(),
            in1_t.options().device_opt(),
            in1_t.options().pinned_memory_opt(),
            at::MemoryFormat::Preserve);
      }
    }
    auto& out_t = p_node->Output(0).toTensor();
    fastResizeToZero(out_t);
    if (p_node->Input(0).isTensor()) {
      if (p_node->Input(1).isTensor()) {
        at::cpu::pow_out(
            out_t, p_node->Input(0).toTensor(), p_node->Input(1).toTensor());
      } else {
        at::cpu::pow_out(
            out_t, p_node->Input(0).toTensor(), p_node->Input(1).toScalar());
      }
    } else {
      at::cpu::pow_out(
          out_t, p_node->Input(0).toScalar(), p_node->Input(1).toTensor());
    }
  };
});
// out variant takes precedence over native
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(
    static_runtime::to_copy,
    aten_to_copy,
    [](Node* n) -> SROperator {
      // support 4- or 5-arg for adindexer/adfinder models
      TORCH_CHECK(n->inputs().size() == 4 || n->inputs().size() == 5);

      return [](ProcessedNode* p_node) {
        const auto& self = p_node->Input(0).toTensor();
        if (p_node->Output(0).isNone()) {
          // handle dtype, layout, and device
          at::ScalarType dtype;
          c10::Layout layout = self.layout();
          c10::Device device = self.device();
          if (p_node->Input(1).isTensor()) {
            const auto& other = p_node->Input(1).toTensor();
            dtype = other.scalar_type();
            layout = other.layout();
            device = other.device();
          } else {
            dtype = p_node->Input(1).toScalarType();
          }
          // handle memory format
          c10::optional<c10::MemoryFormat> memory_format = c10::nullopt;
          if (p_node->inputs().size() == 5) {
            memory_format = p_node->Input(4).toOptional<c10::MemoryFormat>();
          }
          if (memory_format.value_or(c10::MemoryFormat::Preserve) ==
              c10::MemoryFormat::Preserve) {
            if (self.is_non_overlapping_and_dense()) {
              memory_format = c10::nullopt;
            } else {
              memory_format = self.suggest_memory_format();
            }
          }
          // See Note [Explicit nullopt MemoryFormat argument]
          p_node->Output(0) = at::detail::empty_cpu(
              {0}, dtype, layout, self.device(), c10::nullopt, memory_format);
        }

        // ignore input 3 (copy)
        auto non_blocking = p_node->Input(2).toBool(); // non_blocking
        auto& out_t = p_node->Output(0).toTensor();
        fastResizeToZero(out_t);
        at::native::to_copy_out(out_t, self, non_blocking);
      };
    });

// Out variants for view ops are registered to a separate registry because
// their outputs (views) can't participate in memory reuse.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(
    static_runtime::reshape_copy,
    aten_reshape,
    [](Node* n) -> SROperator {
      return [](ProcessedNode* p_node) {
        const auto& self = p_node->Input(0).toTensor(); // self
        const auto proposed_shape = p_node->Input(1).toIntVector(); // shape

        if (p_node->Output(0).isNone()) {
          p_node->Output(0) = create_empty_from(self);
        }
        auto& out = p_node->Output(0).toTensor();
        at::native::reshape_copy_out(out, self, proposed_shape, true);
      };
    });

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(
    static_runtime::flatten_copy,
    aten_flatten,
    [](Node* n) -> SROperator {
      TORCH_CHECK(n->inputs().size() == 3);
      return [](ProcessedNode* p_node) {
        const auto& self = p_node->Input(0).toTensor();
        const auto start_dim = p_node->Input(1).toInt();
        const auto end_dim = p_node->Input(2).toInt();

        if (p_node->Output(0).isNone()) {
          p_node->Output(0) = create_empty_from(self);
        }
        auto& out = p_node->Output(0).toTensor();
        at::native::flatten_copy_out(out, self, start_dim, end_dim);
      };
    });

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::sum, aten_sum, [](Node* n) -> SROperator {
  return [](ProcessedNode* p_node) {
    const at::Tensor& self = p_node->Input(0).toTensor();

    c10::optional<at::ScalarType> dtype = c10::nullopt;
    if (p_node->inputs().size() == 2) {
      // sum(Tensor self, *, ScalarType? dtype=None) -> Tensor
      dtype = p_node->Input(1).toOptional<at::ScalarType>();
    }

    std::vector<int64_t> dim = {};
    bool keepdim = false;
    if (p_node->inputs().size() == 4) {
      // sum.dim_IntList(Tensor self, int[1] dim, bool keepdim=False, *,
      // ScalarType? dtype=None) -> Tensor
      dim = p_node->Input(1).toIntList().vec();
      keepdim = p_node->Input(2).toBool();
      dtype = p_node->Input(3).toOptional<at::ScalarType>();
    }

    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(self);
    }
    auto& output = p_node->Output(0).toTensor();
    fastResizeToZero(output);
    at::native::sum_out(self, dim, keepdim, dtype, output);
  };
});

std::function<void(ProcessedNode*)> getOutOfPlaceOperation(Node* n) {
  auto op_name = n->kind().toQualString();
  if (SROperatorRegistry()->Has(op_name)) {
    return SROperatorRegistry()->Create(op_name)->Generate(n);
  }

  return [](ProcessedNode*) { TORCH_CHECK(0); };
}

std::function<void(ProcessedNode*)> getNativeOperation(Node* n) {
  if (n->kind() == c10::Symbol::fromQualString("aten::transpose")) {
    return [](ProcessedNode* p_node) {
      const auto& in0_t = p_node->Input(0).toTensor();
      const auto in1_i = p_node->Input(1).toInt();
      const auto in2_i = p_node->Input(2).toInt();
      p_node->Output(0) = at::native::transpose(in0_t, in1_i, in2_i);
    };
  } else if (n->kind() == c10::Symbol::fromQualString("aten::flatten")) {
    return [](ProcessedNode* p_node) {
      DCHECK(p_node->inputs().size() == 3);
      const auto& in0_t = p_node->Input(0).toTensor();
      const auto in1_i = p_node->Input(1).toInt();
      const auto in2_i = p_node->Input(2).toInt();
      p_node->Output(0) = at::native::flatten(in0_t, in1_i, in2_i);
    };
  } else if (n->kind() == prim::TupleConstruct) {
    return [](ProcessedNode* p_node) {
      // prepare inputs
      std::vector<IValue> stack;
      const size_t size = p_node->inputs().size();
      stack.reserve(size);
      for (size_t i = 0; i < size; i++) {
        stack.emplace_back(p_node->Input(i));
      }
      // run op
      auto* node = p_node->node();
      const auto& type = node->output()->type()->expect<TupleType>();
      if (type->name().has_value()) {
        namedTupleConstruct(stack, type, node->inputs().size());
      } else {
        tupleConstruct(stack, node->inputs().size());
      }
      // put output back
      p_node->Output(0) = std::move(stack[0]);
    };
  } else if (n->kind() == prim::DictConstruct) {
    return [](ProcessedNode* p_node) {
      // prepare inputs
      std::vector<IValue> stack;
      const size_t size = p_node->inputs().size();
      stack.reserve(size);
      for (size_t i = 0; i < size; i++) {
        stack.emplace_back(p_node->Input(i));
      }
      // run op
      auto* node = p_node->node();
      dictConstruct(
          stack,
          node->output()->type()->expectRef<DictType>(),
          node->inputs().size());
      // put output back
      p_node->Output(0) = std::move(stack[0]);
    };
  } else if (n->kind() == c10::Symbol::fromQualString("aten::__getitem__")) {
    return [](ProcessedNode* p_node) {
      auto dict = p_node->Input(0).toGenericDict();
      auto key = p_node->Input(1);
      auto value = dict.find(key);
      TORCH_CHECK(value != dict.end(), "Key not in dict: ", key);
      p_node->Output(0) = value->value();
    };
  } else if (n->kind() == prim::ListConstruct) {
    return [](ProcessedNode* p_node) {
      // prepare inputs
      std::vector<IValue> stack;
      const size_t size = p_node->inputs().size();
      stack.reserve(size);
      for (size_t i = 0; i < size; i++) {
        stack.emplace_back(p_node->Input(i));
      }
      // run op
      listConstruct(
          stack,
          p_node->node()->output()->type()->expectRef<ListType>(),
          p_node->inputs().size());
      // put output back
      p_node->Output(0) = std::move(stack[0]);
    };
  } else if (n->kind() == prim::ListUnpack) {
    return [](ProcessedNode* p_node) {
      // prepare inputs
      std::vector<IValue> stack;
      const size_t size = p_node->inputs().size();
      stack.reserve(size);
      for (size_t i = 0; i < size; i++) {
        stack.emplace_back(p_node->Input(i));
      }
      // run op
      size_t num_outputs = p_node->outputs().size();
      listUnpack(stack, num_outputs);
      // put output back
      DCHECK_EQ(stack.size(), num_outputs);
      // NOLINTNEXTLINE(clang-diagnostic-sign-compare)
      for (auto i = 0; i < num_outputs; i++) {
        p_node->Output(i) = std::move(stack[i]);
      }
    };
  } else if (n->kind() == c10::Symbol::fromQualString("aten::permute")) {
    return [](ProcessedNode* p_node) {
      const auto& in0_t = p_node->Input(0).toTensor();
      const auto in1_iv = p_node->Input(1).toIntVector();
      p_node->Output(0) = at::native::permute(in0_t, in1_iv);
    };
  } else if (n->kind() == c10::Symbol::fromQualString("aten::reshape")) {
    return [](ProcessedNode* p_node) {
      const auto& in0_t = p_node->Input(0).toTensor();
      const auto in1_iv = p_node->Input(1).toIntVector();
      p_node->Output(0) = at::native::reshape(in0_t, in1_iv);
    };
  } else if (n->kind() == c10::Symbol::fromQualString("aten::slice")) {
    return [](ProcessedNode* p_node) {
      const auto& in0_t = p_node->Input(0).toTensor();
      const auto in1_i = p_node->Input(1).toInt();
      const auto in2_i = p_node->Input(2).toInt();
      const auto in3_i = p_node->Input(3).toInt();
      const auto in4_i = p_node->Input(4).toInt();
      p_node->Output(0) = at::native::slice(in0_t, in1_i, in2_i, in3_i, in4_i);
    };
  } else if (n->kind() == c10::Symbol::fromQualString("aten::narrow")) {
    return [](ProcessedNode* p_node) {
      const auto& self = p_node->Input(0).toTensor(); // self
      const auto dim = p_node->Input(1).toInt(); // dim
      int64_t start = 0;
      if (p_node->Input(2).isScalar()) {
        start = p_node->Input(2).toInt();
      } else {
        auto& t = p_node->Input(2).toTensor();
        start = t.item<int64_t>();
      }
      const auto length = p_node->Input(3).toInt(); // length
      TORCH_CHECK(
          self.dim() > 0, "narrow() cannot be applied to a 0-dim tensor.");
      auto cur_size = self.sizes()[dim];
      if (start != cur_size && start < 0) { // start being the end is valid, but
                                            // not a valid dim specification.
        start = at::maybe_wrap_dim(start, cur_size);
      }
      TORCH_CHECK(
          length >= 0 && start <= cur_size - length,
          "start (",
          start,
          ") + length (",
          length,
          ") exceeds dimension size (",
          cur_size,
          ").");
      p_node->Output(0) =
          at::native::slice(self, dim, start, start + length, 1);
    };
  } else if (n->kind() == c10::Symbol::fromQualString("aten::to")) {
    return [](ProcessedNode* p_node) {
      DCHECK(p_node->inputs().size() == 5);
      const auto& in0_t = p_node->Input(0).toTensor();
      const auto in2_i = p_node->Input(2).toBool();
      const auto in3_i = p_node->Input(3).toBool();
      const auto in4_o = p_node->Input(4).toOptional<at::MemoryFormat>();
      if (p_node->Input(1).isTensor()) {
        // to.other(Tensor self, Tensor other, bool non_blocking=False, bool
        // copy=False, MemoryFormat? memory_format=None) -> Tensor
        const auto in1_t = p_node->Input(1).toTensor();
        p_node->Output(0) = at::native::to(in0_t, in1_t, in2_i, in3_i, in4_o);
      } else {
        // to.dtype(Tensor self, ScalarType dtype, bool non_blocking=False, bool
        // copy=False, MemoryFormat? memory_format=None) -> Tensor
        const auto in1_i = p_node->Input(1).toScalarType();
        p_node->Output(0) = at::native::to(in0_t, in1_i, in2_i, in3_i, in4_o);
      }
    };
  }
  return [](ProcessedNode*) { TORCH_CHECK(0); };
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(
    aten::embedding_bag,
    aten_embedding_bag,
    [](Node* n) -> SROperator {
      return [](ProcessedNode* p_node) {
        // TODO: Support only 9 args once the old signature has been removed.
        TORCH_CHECK(
            p_node->inputs().size() == 8 || p_node->inputs().size() == 9,
            "Expected number of inputs is 8 or 9, but got " +
                std::to_string(p_node->inputs().size()));

        const auto& weight = p_node->Input(0).toTensor();
        const auto& indices = p_node->Input(1).toTensor();
        const auto& offsets = p_node->Input(2).toTensor();
        auto scale_grad_by_freq = p_node->Input(3).toBool();
        auto mode = p_node->Input(4).to<int64_t>();
        auto sparse = p_node->Input(5).toBool();
        auto per_sample_weights = p_node->Input(6).toOptional<at::Tensor>();
        auto include_last_offset = p_node->Input(7).toBool();
        c10::optional<int64_t> padding_idx;
        if (p_node->inputs().size() == 9) {
          if (p_node->Input(8).isNone()) {
            padding_idx = c10::nullopt;
          } else {
            padding_idx = p_node->Input(8).toInt();
          }
        }

        at::native::check_arguments(
            weight,
            indices,
            offsets,
            mode,
            per_sample_weights,
            include_last_offset);

        std::ignore = scale_grad_by_freq;
        std::ignore = sparse;

        if (p_node->Output(0).isNone()) {
          p_node->Output(0) = at::empty(
              {include_last_offset ? offsets.sizes()[0] - 1
                                   : offsets.sizes()[0],
               weight.sizes()[1]},
              weight.options());
        } else {
          at::native::resize_(
              p_node->Output(0).toTensor(),
              {include_last_offset ? offsets.sizes()[0] - 1
                                   : offsets.sizes()[0],
               weight.sizes()[1]},
              c10::nullopt);
        }
        at::Tensor& output = p_node->Output(0).toTensor();

        if (p_node->Output(1).isNone()) {
          p_node->Output(1) = at::empty({0}, offsets.options());
        }
        at::Tensor& offset2bag = p_node->Output(1).toTensor();
        at::native::make_offset2bag_out(
            offset2bag,
            output,
            weight,
            indices,
            offsets,
            mode,
            per_sample_weights,
            padding_idx.value_or(-1));

        if (p_node->Output(2).isNone()) {
          p_node->Output(2) = at::empty(offsets.sizes(), offsets.options());
        }
        at::Tensor& bag_size = p_node->Output(2).toTensor();
        at::native::make_bag_size_out(
            bag_size, offsets, indices, mode, include_last_offset, false);

        if (p_node->Output(3).isNone()) {
          p_node->Output(3) = at::empty(bag_size.sizes(), offsets.options());
        }
        at::Tensor& max_indices = p_node->Output(3).toTensor();
        at::native::make_max_indices_out(
            max_indices,
            weight,
            indices,
            offsets,
            bag_size,
            mode,
            include_last_offset);

        at::native::_embedding_bag_cpu_impl_out(
            output,
            offset2bag,
            bag_size,
            max_indices,
            weight,
            indices,
            offsets,
            mode,
            per_sample_weights,
            include_last_offset,
            padding_idx.value_or(-1));
      };
    });

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::div, aten_div, [](Node* n) -> SROperator {
  return [](ProcessedNode* p_node) {
    const auto& in0_t = p_node->Input(0).toTensor();
    c10::optional<std::string> rounding_mode = c10::nullopt;
    if (p_node->inputs().size() > 2) {
      rounding_mode = p_node->Input(2).toOptional<std::string>();
    }

    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(in0_t);
    }
    auto& out_t = p_node->Output(0).toTensor();
    fastResizeToZero(out_t);

    const auto& in1_t = p_node->Input(1).isTensor()
        ? p_node->Input(1).toTensor()
        : at::native::wrapped_scalar_tensor(p_node->Input(1).toScalar());
    at::cpu::div_out(out_t, in0_t, in1_t, rounding_mode);
  };
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::sub, aten_sub, [](Node* n) -> SROperator {
  return [](ProcessedNode* p_node) {
    const auto& in0_t = p_node->Input(0).toTensor();
    const auto alpha = p_node->Input(2).toScalar();

    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(in0_t);
    }
    auto& out_t = p_node->Output(0).toTensor();
    fastResizeToZero(out_t);

    const auto& in1_t = p_node->Input(1).isTensor()
        ? p_node->Input(1).toTensor()
        : at::native::wrapped_scalar_tensor(p_node->Input(1).toScalar());
    at::cpu::sub_out(out_t, in0_t, in1_t, alpha);
  };
});

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(
    aten::clamp_min,
    aten_clamp_min,
    [](Node* n) -> SROperator {
      return [](ProcessedNode* p_node) {
        const auto& in0_t = p_node->Input(0).toTensor();
        const auto in1_s = p_node->Input(1).toScalar();
        if (p_node->Output(0).isNone()) {
          p_node->Output(0) = create_empty_from(in0_t);
        }
        auto& out_t = p_node->Output(0).toTensor();
        fastResizeToZero(out_t);
        at::native::clamp_min_out(in0_t, in1_s, out_t);
      };
    });

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
REGISTER_OPERATOR_FUNCTOR(aten::argmin, aten_argmin, [](Node* n) -> SROperator {
  return [](ProcessedNode* p_node) {
    const auto& in0_t = p_node->Input(0).toTensor();
    const auto dim = p_node->Input(1).toOptional<int64_t>();
    const auto keepdim = p_node->Input(2).toBool();
    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(in0_t, at::kLong);
    }
    auto& out_t = p_node->Output(0).toTensor();
    fastResizeToZero(out_t);
    at::native::argmin_out(in0_t, dim, keepdim, out_t);
  };
});

REGISTER_OPERATOR_FUNCTOR(
    aten::layer_norm,
    aten_layer_norm,
    [](Node* n) -> SROperator {
      return [](ProcessedNode* p_node) {
        const auto& input = p_node->Input(0).toTensor();
        const auto normalized_shape = p_node->Input(1).toIntVector();
        auto weight_opt = p_node->Input(2).toOptional<at::Tensor>();
        auto bias_opt = p_node->Input(3).toOptional<at::Tensor>();
        float eps = p_node->Input(4).toDouble();

        c10::MaybeOwned<at::Tensor> weight_maybe_owned =
            at::borrow_from_optional_tensor(weight_opt);
        const at::Tensor& weight = *weight_maybe_owned;
        c10::MaybeOwned<at::Tensor> bias_maybe_owned =
            at::borrow_from_optional_tensor(bias_opt);
        const at::Tensor& bias = *bias_maybe_owned;

        auto inputs = at::native::_prepare_layer_norm_inputs(
            input, normalized_shape, weight, bias);
        auto X = std::get<0>(inputs);
        auto gamma = std::get<1>(inputs);
        auto beta = std::get<2>(inputs);
        auto M = std::get<3>(inputs);
        auto N = std::get<4>(inputs);

        if (p_node->Output(0).isNone()) {
          p_node->Output(0) = at::native::empty_like(
              X,
              c10::nullopt /* dtype */,
              c10::nullopt /* layout */,
              c10::nullopt /* device */,
              c10::nullopt /* pin_memory */,
              at::MemoryFormat::Contiguous);
        } else {
          at::native::resize_(
              p_node->Output(0).toTensor(), X.sizes(), c10::nullopt);
        }
        at::Tensor& output = p_node->Output(0).toTensor();
        at::Tensor mean = at::empty({M}, X.options());
        at::Tensor rstd = at::empty({M}, X.options());

        at::native::layer_norm_cpu_out(
            output,
            mean,
            rstd,
            input,
            normalized_shape,
            gamma,
            beta,
            eps,
            M,
            N);
      };
    });

/* Support the following signatures of norm:
 * norm.ScalarOpt_dtype(Tensor self, Scalar? p, *, ScalarType dtype)
 * norm.ScalarOpt_dim_dtype(Tensor self, Scalar? p, int[1] dim, bool keepdim, *,
 *                          ScalarType dtype)
 * norm.ScalarOpt_dim(Tensor self, Scalar? p, int[1] dim, bool keepdim=False)
 */
REGISTER_OPERATOR_FUNCTOR(aten::norm, aten_norm, [](Node* n) -> SROperator {
  TORCH_CHECK(
      n->inputs().size() > 2,
      "Please implement static runtime support for aten::norm 2-arg version");
  auto val_2 = toIValue(n->inputs()[2]);
  if (val_2) {
    TORCH_CHECK(
        val_2->isIntList() || val_2->isInt(),
        "Please implement static runtime support for aten::norm w/ DimnameList");
  }

  return [](ProcessedNode* p_node) {
    const auto& in0_t = p_node->Input(0).toTensor();

    if (p_node->Output(0).isNone()) {
      p_node->Output(0) = create_empty_from(in0_t);
    }
    auto& out_t = p_node->Output(0).toTensor();
    fastResizeToZero(out_t);

    const size_t num_inp = p_node->inputs().size();
    const auto in1_s = p_node->Input(1).toOptional<at::Scalar>();
    if (num_inp == 3) {
      at::native::norm_out(
          in0_t,
          in1_s,
          c10::IntArrayRef{},
          false,
          p_node->Input(2).toScalarType(),
          out_t);
      return;
    }

    if (num_inp > 4) {
      at::native::norm_out(
          in0_t,
          in1_s,
          p_node->Input(2).toIntVector(), // dim
          p_node->Input(3).toBool(), // keepdim
          p_node->Input(4).toScalarType(), // dtype
          out_t);
      return;
    }
    at::native::norm_out(
        in0_t,
        in1_s,
        p_node->Input(2).toIntVector(), // dim
        p_node->Input(3).toBool(), // keepdim
        out_t);
  };
});

} // namespace jit
} // namespace torch
