#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
#ifdef __ANDROID__
#define ffi_prep_cif _frida_ffi_prep_cif
#define ffi_call _frida_ffi_call
#define ffi_closure_alloc _frida_ffi_closure_alloc
#define ffi_closure_free _frida_ffi_closure_free
#define ffi_prep_closure_loc _frida_ffi_prep_closure_loc
#define ffi_get_struct_offsets _frida_ffi_get_struct_offsets
#define ffi_type_void _frida_ffi_type_void
#define ffi_type_uint8 _frida_ffi_type_uint8
#define ffi_type_sint8 _frida_ffi_type_sint8
#define ffi_type_uint16 _frida_ffi_type_uint16
#define ffi_type_sint16 _frida_ffi_type_sint16
#define ffi_type_uint32 _frida_ffi_type_uint32
#define ffi_type_sint32 _frida_ffi_type_sint32
#define ffi_type_uint64 _frida_ffi_type_uint64
#define ffi_type_sint64 _frida_ffi_type_sint64
#define ffi_type_float _frida_ffi_type_float
#define ffi_type_double _frida_ffi_type_double
#define ffi_type_pointer _frida_ffi_type_pointer
#endif
#include <ffi.h>
namespace vulkanscope::unity {
/** @brief Own a libffi type graph; validate every value-type member offset
 * against IL2CPP. */
struct NativeType {
  ffi_type *type;
  ffi_type structure{};
  std::vector<std::shared_ptr<NativeType>> children;
  std::vector<ffi_type *> elements;
  explicit NativeType(ffi_type *value) : type(value) {}
  static std::shared_ptr<NativeType>
  aggregate(std::vector<std::shared_ptr<NativeType>> fields,
            const std::vector<size_t> &offsets, size_t size, size_t alignment);
};
/** @brief Typed replacement closure; Gum must finish all in-flight calls before
 * destruction. */
class NativeHook {
public:
  NativeHook(std::shared_ptr<NativeType> result,
             std::vector<std::shared_ptr<NativeType>> arguments,
             std::map<unsigned, std::vector<uint8_t>> overrides,
             std::vector<uint8_t> resultOverride, bool replaceResult,
             bool skip);
  ~NativeHook();
  NativeHook(const NativeHook &) = delete;
  void *code = nullptr, *original = nullptr;
  std::atomic<uint64_t> calls{0};

private:
  static void invoke(ffi_cif *, void *, void **, void *);
  ffi_cif cif_{};
  ffi_closure *closure_ = nullptr;
  std::shared_ptr<NativeType> result_;
  std::vector<std::shared_ptr<NativeType>> types_;
  std::vector<ffi_type *> arguments_;
  std::map<unsigned, std::vector<uint8_t>> overrides_;
  std::vector<uint8_t> resultOverride_;
  bool replaceResult_, skip_;
};
}
