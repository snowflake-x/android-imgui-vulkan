#include "unity/ffi_hook.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
namespace vulkanscope::unity {
std::shared_ptr<NativeType>
NativeType::aggregate(std::vector<std::shared_ptr<NativeType>> fields,
                      const std::vector<size_t> &offsets, size_t size,
                      size_t alignment) {
  if (fields.empty() || fields.size() > 256 ||
      fields.size() != offsets.size() || size > 4096)
    throw std::runtime_error(
        "Unsupported value-type field count or size for native Hook");
  auto result = std::make_shared<NativeType>(nullptr);
  result->children = std::move(fields);
  for (auto &field : result->children)
    result->elements.push_back(field->type);
  result->elements.push_back(nullptr);
  result->structure.type = FFI_TYPE_STRUCT;
  result->structure.elements = result->elements.data();
  result->type = &result->structure;
  std::vector<size_t> actual(offsets.size());
  if (ffi_get_struct_offsets(FFI_DEFAULT_ABI, result->type, actual.data()) !=
          FFI_OK ||
      actual != offsets || result->type->size != size ||
      result->type->alignment != alignment)
    throw std::runtime_error(
        "IL2CPP struct layout does not match native ABI; Hook rejected");
  return result;
}
NativeHook::NativeHook(std::shared_ptr<NativeType> result,
                       std::vector<std::shared_ptr<NativeType>> arguments,
                       std::map<unsigned, std::vector<uint8_t>> overrides,
                       std::vector<uint8_t> resultOverride, bool replaceResult,
                       bool skip)
    : result_(std::move(result)), types_(std::move(arguments)),
      overrides_(std::move(overrides)),
      resultOverride_(std::move(resultOverride)), replaceResult_(replaceResult),
      skip_(skip) {
  if (types_.size() > 34 || result_->type->size > 4096)
    throw std::runtime_error("Native Hook signature exceeds limits");
  for (auto &t : types_)
    arguments_.push_back(t->type);
  if (ffi_prep_cif(&cif_, FFI_DEFAULT_ABI, arguments_.size(), result_->type,
                   arguments_.data()) != FFI_OK)
    throw std::runtime_error("Unsupported native call signature");
  for (auto &[index, bytes] : overrides_)
    if (index >= types_.size() || bytes.size() != types_[index]->type->size)
      throw std::runtime_error("Native Hook argument size mismatch");
  if (replaceResult_ && resultOverride_.size() != result_->type->size)
    throw std::runtime_error("Native Hook return size mismatch");
  if (skip_ && result_->type != &ffi_type_void && !replaceResult_)
    throw std::runtime_error(
        "Skipping non-void method requires a return value");
  closure_ =
      static_cast<ffi_closure *>(ffi_closure_alloc(sizeof(ffi_closure), &code));
  if (!closure_)
    throw std::runtime_error("Cannot allocate executable Hook closure");
  if (ffi_prep_closure_loc(closure_, &cif_, invoke, this, code) != FFI_OK) {
    ffi_closure_free(closure_);
    closure_ = nullptr;
    throw std::runtime_error("Cannot prepare Hook closure");
  }
}
NativeHook::~NativeHook() {
  if (closure_)
    ffi_closure_free(closure_);
}
void NativeHook::invoke(ffi_cif *, void *result, void **arguments, void *data) {
  auto &hook = *static_cast<NativeHook *>(data);
  ++hook.calls;
  for (auto &[index, bytes] : hook.overrides_)
    memcpy(arguments[index], bytes.data(), bytes.size());
  alignas(16) std::array<uint8_t, 4096> value{};
  if (!hook.skip_)
    ffi_call(&hook.cif_, FFI_FN(hook.original), value.data(), arguments);
  if (hook.replaceResult_)
    memcpy(value.data(), hook.resultOverride_.data(),
           hook.resultOverride_.size());
  if (hook.result_->type != &ffi_type_void)
    memcpy(result, value.data(), hook.result_->type->size);
}
}
