#ifndef CAFFE2_CORE_OPERATOR_H_
#define CAFFE2_CORE_OPERATOR_H_

#include <climits>
#include <cstddef>
#include <typeinfo>
#include <vector>

#include "caffe2/core/blob.h"
#include "caffe2/core/common.h"
#include "caffe2/core/net.h"
#include "caffe2/core/operator_gradient.h"
#include "caffe2/core/operator_schema.h"
#include "caffe2/core/registry.h"
#include "caffe2/core/tensor.h"
#include "caffe2/core/workspace.h"
#include "caffe2/utils/proto_utils.h"
#include "caffe2/proto/caffe2.pb.h"

namespace caffe2 {

class OperatorBase {
 public:
  explicit OperatorBase(const OperatorDef& operator_def, Workspace* ws);
  virtual ~OperatorBase() {}

  // Parameter getters. You can use these to get the arguments that you want.
  bool HasArgument(const string& name) { return (arg_map_.count(name) > 0); }

  // Functions that deal with arguments. Basically, this allows us to map an
  // argument name to a specific type of argument that we are trying to access.
  template <typename T>
  T GetSingleArgument(const string& name, const T& default_value);
  template <typename T>
  bool HasSingleArgumentOfType(const string& name);
  template <typename T>
  vector<T> GetRepeatedArgument(const string& name);

  template <typename MessageType>
  MessageType GetMessageArgument(const string& name) {
    CAFFE_ENFORCE(arg_map_.count(name),
        "Cannot find parameter named ", name);
    MessageType message;
    if (arg_map_[name]->has_s()) {
      CHECK(message.ParseFromString(arg_map_[name]->s()))
          << "Faild to parse content from the string";
    } else {
      VLOG(1) << "Return empty message for parameter " << name;
    }
    return message;
  }
  template <typename MessageType>
  vector<MessageType> GetRepeatedMessageArgument(const string& name) {
    CAFFE_ENFORCE(arg_map_.count(name),
        "Cannot find parameter named ", name);
    vector<MessageType> messages(arg_map_[name]->strings_size());
    for (int i = 0; i < messages.size(); ++i) {
      CHECK(messages[i].ParseFromString(arg_map_[name]->strings(i)))
          << "Faild to parse content from the string";
    }
    return messages;
  }

  // Get the inputs and outputs as specific types.
  template <typename T>
  inline const T& Input(int idx) {
    DCHECK_LT(idx, inputs_.size());
    return inputs_.at(idx)->template Get<T>();
  }

  template <typename T>
  inline T* Output(int idx) {
    DCHECK_LT(idx, outputs_.size());
    return outputs_.at(idx)->template GetMutable<T>();
  }

  template <typename T>
  inline bool InputIsType(int idx) {
    return inputs_.at(idx)->template IsType<T>();
  }

  template <typename T>
  inline bool OutputIsType(int idx) {
    return outputs_.at(idx)->template IsType<T>();
  }

  inline int InputSize() { return inputs_.size(); }
  inline int OutputSize() { return outputs_.size(); }
  inline const vector<const Blob*>& Inputs() const { return inputs_; }
  inline const vector<Blob*>& Outputs() { return outputs_; }

  virtual bool Run() {
    CAFFE_NOT_IMPLEMENTED;
  }
  virtual bool RunAsync() { return Run(); }

  inline const OperatorDef& def() { return operator_def_; }

 private:
  CaffeMap<string, const Argument*> arg_map_;
  OperatorDef operator_def_;
  vector<const Blob*> inputs_;
  vector<Blob*> outputs_;

  DISABLE_COPY_AND_ASSIGN(OperatorBase);
};

// If your operator does not need any specialized contructor or destructor,
// you can simply use this to save two lines of code.
#define USE_SIMPLE_BASE_CTOR_DTOR(name)                                        \
  name(const OperatorDef& operator_def, Workspace* ws)                         \
      : OperatorBase(operator_def, ws) {}                                      \
  virtual ~name() {}

// OP_SINGLE_ARG provides a shorter initialization choice for initialization of
// member variables for the class constructors.
#define OP_SINGLE_ARG(type, name, variable, default)                           \
  variable(OperatorBase::GetSingleArgument<type>(name, (default)))

// INPUT_TAGS and OUTPUT_TAGS are optional features to name the indices of the
// operator's inputs and outputs, in order to avoid confusion. For example, for
// a fully convolution layer that has input, weight and bias, you can define its
// input tags as:
//     INPUT_TAGS(INPUT, WEIGHT, BIAS);
// And in the code, instead of doing
//     auto& weight = Input(1);
// you can now do
//     auto& weight = Input(WEIGHT);
// to make it more clear.
#define INPUT_TAGS(first_input, ...)                                           \
  enum _InputTags { first_input = 0, __VA_ARGS__ }
#define OUTPUT_TAGS(first_input, ...)                                          \
  enum _OutputTags { first_input = 0, __VA_ARGS__ }


// Operator is the class that you usually want to derive, if your operator will
// run on different devices. You should then implement the RunOnDevice()
// function.
template <class Context>
class Operator : public OperatorBase {
 public:
  // The constructor of the operator. Note that you should not do any
  // custom initializations in the constructor; instead, do those in the
  // SetUp() function.
  explicit Operator(const OperatorDef& operator_def, Workspace* ws)
      : OperatorBase(operator_def, ws),
        context_(operator_def.device_option()) {
    // In the constructor, we switch to the device so that the child class
    // constructors will run on that device.
    context_.SwitchToDevice();
  }
  virtual ~Operator() {}

  inline const Tensor<Context>& Input(int idx) {
    return OperatorBase::template Input<Tensor<Context> >(idx); }
  inline Tensor<Context>* Output(int idx) {
    return OperatorBase::template Output<Tensor<Context> >(idx);
  }

  // The run function of Operator switches to the device, and then carries out
  // the actual computation with RunOnDevice(). You should implement RunOnDevice
  // instead of Run().
  bool Run() final {
    try {
      context_.SwitchToDevice();
      bool started = RunOnDevice();
      bool finished = context_.FinishDeviceComputation();
      if (!finished) {
        // FinishDeviceComputation() returning error basically means that there
        // is something wrong with the device (like CUDA) that usually cannot be
        // recovered, so we should log FATAL.
        LOG(FATAL) << "Computation on device returned error in operator\n"
                   << ProtoDebugString(this->def());
      }
      return (started && finished);
    } catch (EnforceNotMet& err) {
      err.AppendMessage("Error from operator " + ProtoDebugString(def()));
      throw;
    }
  }

  bool RunAsync() final {
    try {
      context_.SwitchToDevice();
      return RunOnDevice();
    } catch (EnforceNotMet& err) {
      err.AppendMessage("Error from operator " + ProtoDebugString(def()));
      throw;
    }
  }

  virtual bool RunOnDevice() = 0;

 protected:
  Context context_;
};

#define USE_OPERATOR_BASE_FUNCTIONS                                 \
  /* using override */ using OperatorBase::HasArgument;             \
  /* using override */ using OperatorBase::GetSingleArgument;       \
  /* using override */ using OperatorBase::HasSingleArgumentOfType; \
  /* using override */ using OperatorBase::GetRepeatedArgument;     \
  /* using override */ using OperatorBase::def;                     \
  /* using override */ using OperatorBase::InputIsType;             \
  /* using override */ using OperatorBase::InputSize;               \
  /* using override */ using OperatorBase::OutputSize

#define USE_OPERATOR_FUNCTIONS(context)                   \
  USE_OPERATOR_BASE_FUNCTIONS;                            \
  /* using override */ using Operator<context>::context_; \
  /* using override */ using Operator<context>::Input;    \
  /* using override */ using Operator<context>::Output

#define USE_OPERATOR_CONTEXT_FUNCTIONS USE_OPERATOR_FUNCTIONS(Context)

#define USE_SIMPLE_CTOR_DTOR(name)                                             \
  name(const OperatorDef& operator_def, Workspace* ws)                         \
      : Operator<Context>(operator_def, ws) {}                                 \
  virtual ~name() {}

// Helpers to implement runtime op polymorphism. Often it's convenient to make
// an op work on different input types (e.g. i32 vs i64 indices) or special-case
// it for particular input size (e.g. ScatterWeightedSum for block size of 1
// doesn't need to call Eigen).
//
// DispatchHelper provides compile-time generation of nested "if" statements,
// e.g. `DispatchHelper<FixedSizes<1, 4>>::call(this, block_size);`
// unrolls into:
//   if (block_size == 1) {
//     return DoRunWithSize<1>();
//   } else if (block_size = 4) {
//     return DoRunWithSize<4>();
//   } else {
//     return DoRunWithSize<-1>();
//   }`
//
// DoRunWithSize implementation can use template arguments to do "if" statements
// or proxy to functions in math.h which often provide fixed size
// implementation.
//
// Similarly `TensorTypes<int32_t, int64_t>(this, Input(0))` provides branching
// based on type of the first input and calls DoRunWithType.
//
// Note, that the same instance of Op class is used as the method, not class is
// templated. We might consider adding static class-level polymorphism later.
//
// Convenient macro USE_DISPATCH_HELPER is provided for declaring friendship in
// case DoRunWithSize or DoRunWithType are declared non-public.

#define USE_DISPATCH_HELPER                        \
  template <typename Sizes, typename... ExtraArgs> \
  friend struct DispatchHelper

template <int... Sizes>
struct FixedSizes {};

template <typename... Types>
struct TensorTypes {};

template <typename Sizes, typename... ExtraArgs>
struct DispatchHelper;

template <int FirstSize, int... Sizes, typename... ExtraArgs>
struct DispatchHelper<FixedSizes<FirstSize, Sizes...>, ExtraArgs...> {
  template <typename Op>
  static bool call(Op* op, TIndex size) {
    if (FirstSize == size) {
      return op->template DoRunWithSize<ExtraArgs..., FirstSize>();
    }
    return DispatchHelper<FixedSizes<Sizes...>, ExtraArgs...>::
        template call<Op>(op, size);
  }
};

template <typename... ExtraArgs>
struct DispatchHelper<FixedSizes<>, ExtraArgs...> {
  template <typename Op>
  static bool call(Op* op, TIndex size) {
    return op->template DoRunWithSize<ExtraArgs..., -1>();
  }
};

template <typename FirstType, typename... Types, typename... ExtraArgs>
struct DispatchHelper<TensorTypes<FirstType, Types...>, ExtraArgs...> {
  template <typename Op>
  static bool call(Op* op, const TypeMeta& meta) {
    if (meta.Match<FirstType>()) {
      return op->template DoRunWithType<ExtraArgs..., FirstType>();
    }
    return DispatchHelper<TensorTypes<Types...>, ExtraArgs...>::
        template call<Op>(op, meta);
  }
  template <typename Op, typename Context>
  static bool call(Op* op, const Tensor<Context>& tensor) {
    return call<Op>(op, tensor.meta());
  }
};

template <typename... ExtraArgs>
struct DispatchHelper<TensorTypes<>, ExtraArgs...> {
  template <typename Op>
  static bool call(Op* op, const TypeMeta& meta) {
    CAFFE_THROW("Unsupported type of tensor: ", meta.name());
  }
  template <typename Op, typename Context>
  static bool call(Op* op, const Tensor<Context>& tensor) {
    return call<Op>(op, tensor.meta());
  }
};

// The operator registry. Since we are not expecting a great number of devices,
// we will simply have an if-then type command and allocate the actual
// generation to device-specific registerers.
// Note that although we have CUDA and CUDNN here, the registerers themselves do
// not depend on specific cuda or cudnn libraries. This means that we will be
// able to compile it even when there is no cuda available - we simply do not
// link any cuda or cudnn operators.
CAFFE_DECLARE_REGISTRY(
    CPUOperatorRegistry,
    OperatorBase,
    const OperatorDef&,
    Workspace*);
#define REGISTER_CPU_OPERATOR_CREATOR(key, ...) \
  CAFFE_REGISTER_CREATOR(CPUOperatorRegistry, key, __VA_ARGS__)
#define REGISTER_CPU_OPERATOR(name, ...) \
  CAFFE_REGISTER_CLASS(CPUOperatorRegistry, name, __VA_ARGS__)
#define REGISTER_CPU_OPERATOR_STR(str_name, ...) \
  CAFFE_REGISTER_TYPED_CLASS(CPUOperatorRegistry, str_name, __VA_ARGS__)

#define REGISTER_CPU_OPERATOR_WITH_ENGINE(name, engine, ...) \
  CAFFE_REGISTER_CLASS(CPUOperatorRegistry, name##_ENGINE_##engine, __VA_ARGS__)

CAFFE_DECLARE_REGISTRY(
    CUDAOperatorRegistry,
    OperatorBase,
    const OperatorDef&,
    Workspace*);
#define REGISTER_CUDA_OPERATOR_CREATOR(key, ...) \
  CAFFE_REGISTER_CREATOR(CUDAOperatorRegistry, key, __VA_ARGS__)
#define REGISTER_CUDA_OPERATOR(name, ...) \
  CAFFE_REGISTER_CLASS(CUDAOperatorRegistry, name, __VA_ARGS__)
#define REGISTER_CUDA_OPERATOR_STR(str_name, ...) \
  CAFFE_REGISTER_TYPED_CLASS(CUDAOperatorRegistry, str_name, __VA_ARGS__)

#define REGISTER_CUDA_OPERATOR_WITH_ENGINE(name, engine, ...) \
  CAFFE_REGISTER_CLASS(                                       \
      CUDAOperatorRegistry, name##_ENGINE_##engine, __VA_ARGS__)

// Macros for cudnn since we use it often
#define REGISTER_CUDNN_OPERATOR(name, ...) \
  REGISTER_CUDA_OPERATOR_WITH_ENGINE(name, CUDNN, __VA_ARGS__)

// Creates an operator with the given operator definition.
unique_ptr<OperatorBase> CreateOperator(
    const OperatorDef& operator_def, Workspace* ws);

}  // namespace caffe2

#endif  // CAFFE2_CORE_OPERATOR_H_
