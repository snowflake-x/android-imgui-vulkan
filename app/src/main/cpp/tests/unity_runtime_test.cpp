#include "unity/ffi_hook.h"
#include "unity/runtime.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <frida-gum.h>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>
using namespace vulkanscope::unity;
namespace {
using P = void *;
using C = const void *;
struct Class {
  const char *ns, *name;
  bool value = false;
};
struct Obj {
  Class *klass;
  P monitor = nullptr;
  alignas(16) std::array<unsigned char, 64> data{};
};
struct Field {
  const char *name;
  size_t offset;
  int flags;
  Class *parent;
};
struct Method {
  P entry;
  const char *name;
  int params;
  int flags = 16;
};
Class foo{"Example", "Player"}, integer{"System", "Int32", true},
    thread{"System.Threading", "Thread"},
    execution{"System.Threading", "ExecutionContext"},
    context{"UnityEngine", "UnitySynchronizationContext"},
    callback{"System.Threading", "SendOrPostCallback"};
Obj sc{&context}, ec{&execution}, th{&thread}, boxed{&integer};
Field f{"score", 0, 16, &foo}, contextField{"_syncContext", 0, 0, &execution},
    executionField{"_executionContext", 0, 0, &thread},
    methodPtr{"method_ptr", 0, 0, &callback},
    invokePtr{"invoke_impl", 8, 0, &callback};
void entry() {}
Method add{reinterpret_cast<P>(entry), "Add", 2},
    ctor{reinterpret_cast<P>(entry), ".ctor", 2, 0},
    inv{reinterpret_cast<P>(entry), "Invoke", 1, 0},
    post{reinterpret_cast<P>(entry), "Post", 2, 0};
int score = 42, invocations = 0;
bool missing = false, noContext = false;
std::vector<std::unique_ptr<Obj>> allocated;
std::map<uint32_t, P> handles;
uint32_t nextHandle = 1;
void require(bool condition) {
  if (!condition)
    throw std::runtime_error("Unity runtime assertion failed");
}
P stub() { return nullptr; }
P domain_get() { return &foo; }
P thread_attach(P) { return &th; }
void thread_detach(P) {}
P *thread_get_all_attached_threads(size_t *n) {
  static P t[]{&th};
  *n = noContext ? 0 : 1;
  return t;
}
C *domain_get_assemblies(P, size_t *n) {
  static C all[]{&foo};
  *n = 1;
  return all;
}
C assembly_get_image(C p) { return p; }
const char *image_get_name(C) { return "Assembly-CSharp"; }
size_t image_get_class_count(C) { return 1; }
P image_get_class(C, size_t) { return &foo; }
P class_from_name(C, const char *, const char *n) {
  if (std::string(n) == "SendOrPostCallback")
    return &callback;
  return nullptr;
}
const char *class_get_name(P c) { return static_cast<Class *>(c)->name; }
const char *class_get_namespace(P c) { return static_cast<Class *>(c)->ns; }
P class_get_parent(P) { return nullptr; }
C class_get_type(P c) { return c; }
P class_from_type(C t) { return const_cast<P>(t); }
bool class_is_enum(P) { return false; }
C class_enum_basetype(P) { return &integer; }
bool class_is_valuetype(P c) { return static_cast<Class *>(c)->value; }
bool class_is_assignable_from(P a, P b) { return a == b; }
int32_t class_value_size(P, uint32_t *align) {
  *align = 4;
  return 4;
}
P class_get_fields(P c, P *it) {
  if (c == &foo && !*it) {
    *it = &f;
    return &f;
  }
  return nullptr;
}
C class_get_methods(P c, P *it) {
  if (c == &foo && !*it) {
    *it = &add;
    return &add;
  }
  return nullptr;
}
P class_get_field_from_name(P c, const char *n) {
  for (auto field :
       {&f, &contextField, &executionField, &methodPtr, &invokePtr})
    if (field->parent == c && std::string(n) == field->name)
      return field;
  return nullptr;
}
C class_get_method_from_name(P c, const char *n, int) {
  if (c == &callback) {
    if (std::string(n) == "Invoke")
      return &inv;
    if (std::string(n) == ".ctor")
      return &ctor;
  }
  if (c == &context)
    return &post;
  return nullptr;
}
const char *field_get_name(P p) { return static_cast<Field *>(p)->name; }
C field_get_type(P) { return &integer; }
size_t field_get_offset(P p) { return static_cast<Field *>(p)->offset; }
int field_get_flags(P p) { return static_cast<Field *>(p)->flags; }
P field_get_parent(P p) { return static_cast<Field *>(p)->parent; }
void field_get_value(P o, P p, P out) {
  memcpy(out,
         static_cast<Obj *>(o)->data.data() + static_cast<Field *>(p)->offset,
         sizeof(P));
}
void field_set_value(P o, P p, P value) {
  memcpy(static_cast<Obj *>(o)->data.data() + static_cast<Field *>(p)->offset,
         value, sizeof(P));
}
void field_static_get_value(P, P out) { memcpy(out, &score, 4); }
void field_static_set_value(P, P value) { memcpy(&score, value, 4); }
const char *method_get_name(C m) {
  return static_cast<const Method *>(m)->name;
}
uint32_t method_get_param_count(C m) {
  return static_cast<const Method *>(m)->params;
}
C method_get_param(C, uint32_t) { return &integer; }
const char *method_get_param_name(C, uint32_t) { return "value"; }
C method_get_return_type(C) { return &integer; }
uint32_t method_get_flags(C m, uint32_t *flags) {
  *flags = 0;
  return static_cast<const Method *>(m)->flags;
}
P method_get_class(C) { return &foo; }
bool method_is_generic(C) { return false; }
bool method_is_inflated(C) { return false; }
int type_get_type(C) { return 8; }
char *type_get_name(C) { return strdup("System.Int32"); }
bool type_is_byref(C) { return false; }
P object_get_class(P o) { return static_cast<Obj *>(o)->klass; }
P object_new(P c) {
  allocated.push_back(std::make_unique<Obj>());
  allocated.back()->klass = static_cast<Class *>(c);
  return allocated.back().get();
}
P object_unbox(P o) { return static_cast<Obj *>(o)->data.data(); }
P runtime_invoke(C m, P, P *args, P *ex) {
  *ex = nullptr;
  if (m == &post) {
    auto d = static_cast<Obj *>(args[0]);
    P cb = nullptr;
    memcpy(&cb, d->data.data(), sizeof(P));
    reinterpret_cast<void (*)(P, P, C)>(cb)(d, args[1], nullptr);
  }
  if (m == &add) {
    ++invocations;
    int a, b;
    memcpy(&a, args[0], 4);
    memcpy(&b, args[1], 4);
    int value = a + b;
    memcpy(boxed.data.data(), &value, 4);
    return &boxed;
  }
  return nullptr;
}
void runtime_class_init(P) {}
uint32_t gchandle_new(P p, bool) {
  auto h = nextHandle++;
  handles[h] = p;
  return h;
}
P gchandle_get_target(uint32_t h) { return handles.at(h); }
void gchandle_free(uint32_t h) { handles.erase(h); }
void free_value(P p) { free(p); }
}
GumModule *gum_process_find_module_by_name(const char *) {
  return new GumModule;
}
GumAddress gum_module_find_export_by_name(GumModule *, const char *full) {
  std::string name = full + 7;
  if (missing && name == "runtime_invoke")
    return 0;
#define MAP(n)                                                                 \
  if (name == #n)                                                              \
  return reinterpret_cast<GumAddress>(n)
  MAP(domain_get);
  MAP(thread_attach);
  MAP(thread_detach);
  MAP(thread_get_all_attached_threads);
  MAP(domain_get_assemblies);
  MAP(assembly_get_image);
  MAP(image_get_name);
  MAP(image_get_class_count);
  MAP(image_get_class);
  MAP(class_from_name);
  MAP(class_get_name);
  MAP(class_get_namespace);
  MAP(class_get_parent);
  MAP(class_get_type);
  MAP(class_from_type);
  MAP(class_is_valuetype);
  MAP(class_is_enum);
  MAP(class_enum_basetype);
  MAP(class_is_assignable_from);
  MAP(class_value_size);
  MAP(class_get_fields);
  MAP(class_get_methods);
  MAP(class_get_field_from_name);
  MAP(class_get_method_from_name);
  MAP(field_get_name);
  MAP(field_get_type);
  MAP(field_get_offset);
  MAP(field_get_flags);
  MAP(field_get_parent);
  MAP(field_get_value);
  MAP(field_set_value);
  MAP(field_static_get_value);
  MAP(field_static_set_value);
  MAP(method_get_name);
  MAP(method_get_param_count);
  MAP(method_get_param);
  MAP(method_get_param_name);
  MAP(method_get_return_type);
  MAP(method_get_flags);
  MAP(method_get_class);
  MAP(method_is_generic);
  MAP(method_is_inflated);
  MAP(type_get_type);
  MAP(type_get_name);
  MAP(type_is_byref);
  MAP(object_get_class);
  MAP(object_new);
  MAP(object_unbox);
  MAP(runtime_invoke);
  MAP(runtime_class_init);
  MAP(gchandle_new);
  MAP(gchandle_get_target);
  MAP(gchandle_free);
#undef MAP
  if (name == "free")
    return reinterpret_cast<GumAddress>(free_value);
  return reinterpret_cast<GumAddress>(stub);
}
namespace {
int originalCalls = 0;
float multiply(float a, float b) {
  ++originalCalls;
  return a * b;
}
struct Vec3 {
  float x, y, z;
};
Vec3 translate(Vec3 v, float delta) {
  return {v.x + delta, v.y + delta, v.z + delta};
}
int8_t smallInteger() { return -3; }
template <class T> std::vector<uint8_t> encoded(T value) {
  std::vector<uint8_t> out(sizeof(T));
  memcpy(out.data(), &value, sizeof(value));
  return out;
}
void nativeClosures() {
  auto f = std::make_shared<NativeType>(&ffi_type_float);
  NativeHook hook(f, {f, f}, {{1, encoded(5.0f)}}, {}, false, false);
  hook.original = reinterpret_cast<void *>(multiply);
  require(reinterpret_cast<float (*)(float, float)>(hook.code)(2.0f, 3.0f) ==
          10.0f);
  require(originalCalls == 1 && hook.calls == 1);
  NativeHook skip(f, {f, f}, {}, encoded(7.5f), true, true);
  skip.original = reinterpret_cast<void *>(multiply);
  require(reinterpret_cast<float (*)(float, float)>(skip.code)(2, 3) == 7.5f &&
          originalCalls == 1);
  auto vector =
      NativeType::aggregate({f, f, f}, {0, 4, 8}, sizeof(Vec3), alignof(Vec3));
  NativeHook structure(vector, {vector, f}, {{1, encoded(2.0f)}}, {}, false,
                       false);
  structure.original = reinterpret_cast<void *>(translate);
  auto v =
      reinterpret_cast<Vec3 (*)(Vec3, float)>(structure.code)({1, 2, 3}, 0);
  require(v.x == 3 && v.y == 4 && v.z == 5);
  NativeHook returned(vector, {vector, f}, {}, encoded(Vec3{7, 8, 9}), true,
                      true);
  v = reinterpret_cast<Vec3 (*)(Vec3, float)>(returned.code)({1, 2, 3}, 0);
  require(v.x == 7 && v.z == 9);
  auto byte = std::make_shared<NativeType>(&ffi_type_sint8);
  NativeHook narrow(byte, {}, {}, encoded(int8_t(-5)), true, false);
  narrow.original = reinterpret_cast<void *>(smallInteger);
  require(reinterpret_cast<int8_t (*)()>(narrow.code)() == -5);
  bool rejected = false;
  try {
    NativeType::aggregate({f, f, f}, {0, 8, 12}, 16, 4);
  } catch (...) {
    rejected = true;
  }
  require(rejected);
}
}
int main() {
  nativeClosures();
  P e = &ec, s = &sc;
  memcpy(th.data.data(), &e, sizeof(P));
  memcpy(ec.data.data(), &s, sizeof(P));
  {
    Runtime r;
    int raw = 123;
    std::ostringstream rawAddress;
    rawAddress << "0x" << std::hex << reinterpret_cast<uintptr_t>(&raw);
    auto bytes = r.request(
        {{"op", "readMemory"}, {"address", rawAddress.str()}, {"size", 4}});
    require(bytes["bytes"] == "7b000000");
    r.request({{"op", "writeMemory"},
               {"address", rawAddress.str()},
               {"bytes", "2a000000"}});
    require(raw == 42);
    auto assemblies = r.request({{"op", "connect"}});
    auto image = assemblies["rows"][0]["id"];
    auto types = r.request({{"op", "classes"}, {"image", image}});
    auto klass = types["rows"][0]["id"];
    auto members = r.request({{"op", "members"}, {"class", klass}});
    auto field = members["fields"][0]["id"],
         method = members["methods"][0]["id"];
    require(r.request({{"op", "read"}, {"field", field}})["value"] == 42);
    require(r.request(
                {{"op", "write"}, {"field", field}, {"value", 99}})["value"] ==
            99);
    auto invalid =
        r.request({{"op", "write"}, {"field", field}, {"value", 2147483648LL}});
    require(invalid.contains("error") && score == 99);
    auto result = r.request(
        {{"op", "invoke"}, {"method", method}, {"args", Json::array({2, 3})}});
    require(result["value"] == 5 && invocations == 1);
    require(
        r.request(
             {{"op", "invoke"}, {"method", method}, {"args", Json::array({2})}})
            .contains("error") &&
        invocations == 1);
    require(r.request({{"op", "hook"}, {"method", method}, {"return", 5}})
                .contains("hooked"));
    require(r.request({{"op", "hooks"}})["rows"].size() == 1);
    r.request({{"op", "unhook"}, {"method", method}});
    require(r.request({{"op", "hooks"}})["rows"].empty());
    bool rejected = false;
    try {
      r.request({{"op", "classes"}, {"image", "stale"}});
    } catch (...) {
      rejected = true;
    }
    require(rejected);
  }
  require(handles.empty());
  missing = true;
  bool failed = false;
  try {
    Runtime r;
  } catch (...) {
    failed = true;
  }
  require(failed);
  std::cout << "Unity runtime fixture passed (synthetic IL2CPP; not device "
               "validation)\n";
}
