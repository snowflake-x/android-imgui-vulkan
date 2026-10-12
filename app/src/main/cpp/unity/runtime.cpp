#include "unity/runtime.h"
#include "unity/ffi_hook.h"
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <frida-gum.h>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace vulkanscope::unity {
namespace {
using P = void *;
using C = const void *;
[[noreturn]] void fail(const std::string &s) { throw std::runtime_error(s); }
std::string address(C p) {
  std::ostringstream s;
  s << "0x" << std::hex << reinterpret_cast<uintptr_t>(p);
  return s.str();
}
struct Api {
  GumModule *module = nullptr;
#define API(ret, name, ...) ret (*name)(__VA_ARGS__) = nullptr
  API(P, domain_get);
  API(P, thread_attach, P);
  API(void, thread_detach, P);
  API(P *, thread_get_all_attached_threads, size_t *);
  API(C *, domain_get_assemblies, P, size_t *);
  API(C, assembly_get_image, C);
  API(const char *, image_get_name, C);
  API(size_t, image_get_class_count, C);
  API(P, image_get_class, C, size_t);
  API(P, class_from_name, C, const char *, const char *);
  API(const char *, class_get_name, P);
  API(const char *, class_get_namespace, P);
  API(P, class_get_parent, P);
  API(C, class_get_type, P);
  API(bool, class_is_valuetype, P);
  API(bool, class_is_enum, P);
  API(C, class_enum_basetype, P);
  API(bool, class_is_assignable_from, P, P);
  API(P, class_from_type, C);
  API(int32_t, class_value_size, P, uint32_t *);
  API(P, class_get_fields, P, P *);
  API(C, class_get_methods, P, P *);
  API(P, class_get_field_from_name, P, const char *);
  API(C, class_get_method_from_name, P, const char *, int);
  API(const char *, field_get_name, P);
  API(C, field_get_type, P);
  API(size_t, field_get_offset, P);
  API(int, field_get_flags, P);
  API(P, field_get_parent, P);
  API(void, field_get_value, P, P, P);
  API(void, field_set_value, P, P, P);
  API(void, field_static_get_value, P, P);
  API(void, field_static_set_value, P, P);
  API(const char *, method_get_name, C);
  API(uint32_t, method_get_param_count, C);
  API(C, method_get_param, C, uint32_t);
  API(const char *, method_get_param_name, C, uint32_t);
  API(C, method_get_return_type, C);
  API(uint32_t, method_get_flags, C, uint32_t *);
  API(P, method_get_class, C);
  API(bool, method_is_generic, C);
  API(bool, method_is_inflated, C);
  API(int, type_get_type, C);
  API(char *, type_get_name, C);
  API(bool, type_is_byref, C);
  API(P, type_get_object, C);
  API(P, object_get_class, P);
  API(P, object_new, P);
  API(P, object_unbox, P);
  API(P, runtime_invoke, C, P, P *, P *);
  API(void, runtime_class_init, P);
  API(P, string_new, const char *);
  API(int32_t, string_length, P);
  API(const uint16_t *, string_chars, P);
  API(uint32_t, array_length, P);
  API(uint32_t, gchandle_new, P, bool);
  API(P, gchandle_get_target, uint32_t);
  API(void, gchandle_free, uint32_t);
  API(void, free, P);
  API(void, format_exception, P, char *, int);
#undef API
  Api() {
    module = gum_process_find_module_by_name("libil2cpp.so");
    if (!module)
      fail("libil2cpp.so is not loaded; select an IL2CPP process after Unity "
           "initialization");
#define LOAD(name)                                                             \
  name = reinterpret_cast<decltype(name)>(                                     \
      gum_module_find_export_by_name(module, "il2cpp_" #name));                \
  if (!name) {                                                                 \
    g_object_unref(module);                                                    \
    module = nullptr;                                                          \
    fail("Missing IL2CPP export: " #name);                                     \
  }
    LOAD(domain_get);
    LOAD(thread_attach);
    LOAD(thread_detach);
    LOAD(domain_get_assemblies);
    LOAD(assembly_get_image);
    LOAD(image_get_name);
    LOAD(image_get_class_count);
    LOAD(image_get_class);
    LOAD(class_from_name);
    LOAD(class_get_name);
    LOAD(class_get_namespace);
    LOAD(class_get_parent);
    LOAD(class_get_type);
    LOAD(class_is_valuetype);
    LOAD(class_is_enum);
    LOAD(class_enum_basetype);
    LOAD(class_is_assignable_from);
    LOAD(class_from_type);
    LOAD(class_value_size);
    LOAD(class_get_fields);
    LOAD(class_get_methods);
    LOAD(class_get_field_from_name);
    LOAD(class_get_method_from_name);
    LOAD(field_get_name);
    LOAD(field_get_type);
    LOAD(field_get_offset);
    LOAD(field_get_flags);
    LOAD(field_get_parent);
    LOAD(field_get_value);
    LOAD(field_set_value);
    LOAD(field_static_get_value);
    LOAD(field_static_set_value);
    LOAD(method_get_name);
    LOAD(method_get_param_count);
    LOAD(method_get_param);
    LOAD(method_get_param_name);
    LOAD(method_get_return_type);
    LOAD(method_get_flags);
    LOAD(method_get_class);
    LOAD(method_is_generic);
    LOAD(method_is_inflated);
    LOAD(type_get_type);
    LOAD(type_get_name);
    LOAD(type_is_byref);
    LOAD(type_get_object);
    LOAD(object_get_class);
    LOAD(object_new);
    LOAD(object_unbox);
    LOAD(runtime_invoke);
    LOAD(runtime_class_init);
    LOAD(string_new);
    LOAD(string_length);
    LOAD(string_chars);
    LOAD(array_length);
    LOAD(gchandle_new);
    LOAD(gchandle_get_target);
    LOAD(gchandle_free);
    LOAD(free);
    LOAD(format_exception);
#undef LOAD
    thread_get_all_attached_threads =
        reinterpret_cast<decltype(thread_get_all_attached_threads)>(
            gum_module_find_export_by_name(
                module, "il2cpp_thread_get_all_attached_threads"));
  }
  ~Api() {
    if (module)
      g_object_unref(module);
  }
};
/** @brief Native callback code is resident; cancelled queued delegates never
 * access a destroyed runtime. */
struct Task {
  std::mutex mutex;
  std::condition_variable ready;
  bool running = false, done = false, cancelled = false;
  std::function<Json()> work;
  Json result;
};
std::mutex tasksMutex;
std::map<uintptr_t, std::shared_ptr<Task>> tasks;
void scheduled(P, P state, C) {
  std::shared_ptr<Task> task;
  {
    std::lock_guard<std::mutex> lock(tasksMutex);
    auto it = tasks.find(reinterpret_cast<uintptr_t>(state));
    if (it == tasks.end())
      return;
    task = it->second;
    tasks.erase(it);
  }
  std::unique_lock<std::mutex> lock(task->mutex);
  if (task->cancelled)
    return;
  task->running = true;
  lock.unlock();
  Json result;
  try {
    result = task->work();
  } catch (const std::exception &e) {
    result = {{"error", e.what()}};
  }
  lock.lock();
  task->result = std::move(result);
  task->work = {};
  task->done = true;
  task->ready.notify_all();
}
struct Hook {
  GumInvocationListener *listener = nullptr;
  std::atomic<uint64_t> calls{0};
  std::unique_ptr<NativeHook> typed;
  P entry = nullptr;
  std::vector<uint32_t> roots;
  void (*freeRoot)(uint32_t) = nullptr;
  ~Hook() {
    for (auto h : roots)
      freeRoot(h);
  }
};
/** @brief Keep closures and pinned overrides alive if an original call has not
 * returned after detach. */
std::vector<Hook *> retiredHooks;
void enter(GumInvocationContext *, gpointer data) {
  ++static_cast<Hook *>(data)->calls;
}
}

struct Runtime::Impl {
  Api a;
  P attached = nullptr;
  std::map<std::string, C> images, methods;
  std::map<std::string, P> classes, fields;
  std::map<std::string, uint32_t> objects;
  std::map<std::string, Hook *> hooks;
  GumInterceptor *interceptor = nullptr;
  uint64_t nextObject = 1;
  uint32_t contextHandle = 0;
  explicit Impl() {
    auto domain = a.domain_get();
    if (!domain)
      fail("IL2CPP domain not initialized");
    attached = a.thread_attach(domain);
    if (!attached)
      fail("Cannot attach agent worker to IL2CPP");
    interceptor = gum_interceptor_obtain();
  }
  ~Impl() {
    for (auto &[id, h] : hooks)
      removeHook(h);
    for (auto &[id, h] : objects)
      a.gchandle_free(h);
    if (contextHandle)
      a.gchandle_free(contextHandle);
    if (interceptor)
      g_object_unref(interceptor);
    if (attached)
      a.thread_detach(attached);
  }
  void removeHook(Hook *hook) {
    if (hook->listener) {
      gum_interceptor_detach(interceptor, hook->listener);
      g_object_unref(hook->listener);
      return;
    }
    gum_interceptor_revert(interceptor, hook->entry);
    retiredHooks.push_back(hook);
    for (int i = 0; i < 20; ++i) {
      if (gum_interceptor_flush(interceptor)) {
        for (auto retired : retiredHooks)
          delete retired;
        retiredHooks.clear();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  std::shared_ptr<NativeType> nativeType(C type, unsigned depth = 0) {
    if (depth > 16)
      fail("Value-type nesting exceeds Hook limit");
    auto scalar = [](ffi_type *type) {
      return std::make_shared<NativeType>(type);
    };
    if (a.type_is_byref(type))
      return scalar(&ffi_type_pointer);
    switch (a.type_get_type(type)) {
    case 1:
      return scalar(&ffi_type_void);
    case 2:
    case 5:
      return scalar(&ffi_type_uint8);
    case 4:
      return scalar(&ffi_type_sint8);
    case 3:
    case 7:
      return scalar(&ffi_type_uint16);
    case 6:
      return scalar(&ffi_type_sint16);
    case 8:
      return scalar(&ffi_type_sint32);
    case 9:
      return scalar(&ffi_type_uint32);
    case 10:
    case 24:
      return scalar(&ffi_type_sint64);
    case 11:
    case 25:
      return scalar(&ffi_type_uint64);
    case 12:
      return scalar(&ffi_type_float);
    case 13:
      return scalar(&ffi_type_double);
    default:
      break;
    }
    auto kind = a.type_get_type(type);
    if (kind == 15 || kind == 27)
      return scalar(&ffi_type_pointer);
    if (kind != 14 && kind != 17 && kind != 18 && kind != 20 && kind != 21 &&
        kind != 28 && kind != 29)
      fail("Unsupported native Hook parameter type");
    auto c = a.class_from_type(type);
    if (!c || !a.class_is_valuetype(c))
      return scalar(&ffi_type_pointer);
    if (a.class_is_enum(c))
      return nativeType(a.class_enum_basetype(c), depth + 1);
    std::vector<std::pair<size_t, std::shared_ptr<NativeType>>> members;
    P it = nullptr;
    while (auto f = a.class_get_fields(c, &it)) {
      if (a.field_get_flags(f) & 16)
        continue;
      auto offset = a.field_get_offset(f);
      if (offset < sizeof(P) * 2 || members.size() >= 256)
        fail("Unsupported value-type member offset");
      members.emplace_back(offset - sizeof(P) * 2,
                           nativeType(a.field_get_type(f), depth + 1));
    }
    std::sort(members.begin(), members.end(),
              [](const auto &x, const auto &y) { return x.first < y.first; });
    uint32_t alignment = 0;
    auto size = a.class_value_size(c, &alignment);
    if (size <= 0 || size > 4096)
      fail("Unsupported native value-type size");
    std::vector<std::shared_ptr<NativeType>> types;
    std::vector<size_t> offsets;
    for (auto &member : members) {
      offsets.push_back(member.first);
      types.push_back(std::move(member.second));
    }
    if (types.empty() && size == 1 && alignment == 1) {
      types.push_back(scalar(&ffi_type_uint8));
      offsets.push_back(0);
    }
    return NativeType::aggregate(std::move(types), offsets, size, alignment);
  }
  std::string typeName(C t) {
    char *s = a.type_get_name(t);
    std::string r = s ? s : "?";
    if (s)
      a.free(s);
    return r;
  }
  std::string className(P c) {
    return std::string(a.class_get_namespace(c)) + "." + a.class_get_name(c);
  }
  template <class Map> auto lookup(Map &map, const Json &j, const char *key) {
    auto id = j.at(key).template get<std::string>();
    auto it = map.find(id);
    if (it == map.end())
      fail(std::string("Unknown or expired ") + key + "; refresh this session");
    return it->second;
  }
  P object(const Json &j) {
    if (j.is_null() || j == "")
      return nullptr;
    auto it = objects.find(j.get<std::string>());
    if (it == objects.end())
      fail("Unknown object handle");
    auto o = a.gchandle_get_target(it->second);
    if (!o)
      fail("Object handle expired");
    return o;
  }
  Json hold(P o) {
    if (!o)
      return nullptr;
    for (auto &[id, h] : objects)
      if (a.gchandle_get_target(h) == o)
        return {{"object", id},
                {"address", address(o)},
                {"type", className(a.object_get_class(o))},
                {"class", address(a.object_get_class(o))}};
    if (objects.size() >= 512)
      fail("512 retained objects reached; release objects or disconnect");
    auto id = std::to_string(nextObject++);
    auto h = a.gchandle_new(o, true);
    if (!h)
      fail("Cannot retain object");
    objects[id] = h;
    auto c = a.object_get_class(o);
    classes[address(c)] = c;
    return {{"object", id},
            {"address", address(o)},
            {"type", className(c)},
            {"class", address(c)}};
  }
  P invoke(C m, P o, std::vector<P> args = {}) {
    if (!m)
      fail("Managed method unavailable in this Unity build");
    P ex = nullptr;
    P r = a.runtime_invoke(m, o, args.empty() ? nullptr : args.data(), &ex);
    if (ex) {
      char msg[2048]{};
      a.format_exception(ex, msg, sizeof(msg));
      fail(msg);
    }
    return r;
  }
  P findClass(const char *ns, const char *name) {
    size_t count = 0;
    auto all = a.domain_get_assemblies(a.domain_get(), &count);
    if (count > 8192)
      fail("Assembly count exceeds bound");
    for (size_t i = 0; i < count; ++i) {
      auto c = a.class_from_name(a.assembly_get_image(all[i]), ns, name);
      if (c)
        return c;
    }
    fail(std::string("Class unavailable: ") + ns + "." + name);
  }
  P field(P c, const char *name) {
    for (; c; c = a.class_get_parent(c))
      if (auto f = a.class_get_field_from_name(c, name))
        return f;
    return nullptr;
  }
  C method(P c, const char *name, int n) {
    for (; c; c = a.class_get_parent(c))
      if (auto m = a.class_get_method_from_name(c, name, n))
        return m;
    return nullptr;
  }
  C methodWithType(P c, const char *name, const char *parameterType) {
    P it = nullptr;
    while (auto m = a.class_get_methods(c, &it))
      if (std::string(a.method_get_name(m)) == name &&
          a.method_get_param_count(m) == 1 &&
          typeName(a.method_get_param(m, 0)) == parameterType)
        return m;
    fail(std::string("Missing overload: ") + name + "(" + parameterType + ")");
  }
  P reference(P o, const char *name) {
    auto f = field(a.object_get_class(o), name);
    P v = nullptr;
    if (f)
      a.field_get_value(o, f, &v);
    return v;
  }
  P synchronizationContext() {
    if (contextHandle)
      return a.gchandle_get_target(contextHandle);
    if (!a.thread_get_all_attached_threads)
      fail("This Unity runtime does not expose attached threads; main-thread "
           "scheduling unavailable");
    size_t count = 0;
    auto threads = a.thread_get_all_attached_threads(&count);
    if (count > 4096)
      fail("Invalid attached-thread count");
    for (size_t i = 0; i < count; ++i) {
      auto t = threads[i];
      auto ec = reference(t, "_executionContext");
      if (!ec)
        ec = reference(t, "executionContext");
      if (!ec)
        ec = reference(t, "m_ExecutionContext");
      if (!ec) {
        auto getter =
            method(a.object_get_class(t), "GetMutableExecutionContext", 0);
        if (!getter)
          getter = method(a.object_get_class(t), "get_ExecutionContext", 0);
        if (getter)
          ec = invoke(getter, t);
      }
      if (!ec)
        continue;
      auto sc = reference(ec, "_syncContext");
      if (!sc) {
        auto getter =
            method(a.object_get_class(ec), "get_SynchronizationContext", 0);
        if (getter)
          sc = invoke(getter, ec);
      }
      if (sc && className(a.object_get_class(sc)) ==
                    "UnityEngine.UnitySynchronizationContext") {
        contextHandle = a.gchandle_new(sc, true);
        return sc;
      }
    }
    fail("UnitySynchronizationContext unavailable; resume the game and retry");
  }
  Json onMain(std::function<Json()> fn) {
    P sc = synchronizationContext();
    P dc = findClass("System.Threading", "SendOrPostCallback");
    P delegate = a.object_new(dc);
    if (!delegate)
      fail("Cannot allocate main-thread delegate");
    auto h = a.gchandle_new(delegate, true);
    if (!h)
      fail("Cannot retain main-thread delegate");
    auto task = std::make_shared<Task>();
    task->work = std::move(fn);
    auto id = reinterpret_cast<uintptr_t>(delegate);
    try {
      C inv = method(dc, "Invoke", 1);
      P methodInfo = const_cast<P>(inv);
      invoke(method(dc, ".ctor", 2), delegate, {delegate, &methodInfo});
      P callback = reinterpret_cast<P>(scheduled);
      auto mp = field(dc, "method_ptr"), ip = field(dc, "invoke_impl");
      if (!mp || !ip)
        fail("Unsupported managed delegate layout");
      a.field_set_value(delegate, mp, &callback);
      a.field_set_value(delegate, ip, &callback);
      {
        std::lock_guard<std::mutex> l(tasksMutex);
        tasks[id] = task;
      }
      invoke(method(a.object_get_class(sc), "Post", 2), sc,
             {delegate, delegate});
      std::unique_lock<std::mutex> l(task->mutex);
      if (!task->ready.wait_for(l, std::chrono::seconds(8),
                                [&] { return task->done; })) {
        if (!task->running) {
          task->cancelled = true;
          task->work = {};
          l.unlock();
          std::lock_guard<std::mutex> guard(tasksMutex);
          tasks.erase(id);
          fail("Main-thread request expired before execution; bring the game "
               "to foreground");
        }
        task->ready.wait(l, [&] { return task->done; });
      }
      a.gchandle_free(h);
      return task->result;
    } catch (...) {
      a.gchandle_free(h);
      std::lock_guard<std::mutex> l(tasksMutex);
      tasks.erase(id);
      throw;
    }
  }
  std::string string(P o) {
    if (!o)
      return "";
    auto n = a.string_length(o);
    if (n < 0 || n > 65536)
      fail("String exceeds inspection bound");
    auto p = a.string_chars(o);
    std::string s;
    for (int i = 0; i < n; ++i) {
      uint32_t c = p[i];
      if (c >= 0xd800 && c <= 0xdbff && i + 1 < n && p[i + 1] >= 0xdc00 &&
          p[i + 1] <= 0xdfff)
        c = 0x10000 + ((c - 0xd800) << 10) + (p[++i] - 0xdc00);
      if (c < 128)
        s += char(c);
      else if (c < 2048) {
        s += char(0xc0 | (c >> 6));
        s += char(0x80 | (c & 63));
      } else if (c < 65536) {
        s += char(0xe0 | (c >> 12));
        s += char(0x80 | ((c >> 6) & 63));
        s += char(0x80 | (c & 63));
      } else {
        s += char(0xf0 | (c >> 18));
        s += char(0x80 | ((c >> 12) & 63));
        s += char(0x80 | ((c >> 6) & 63));
        s += char(0x80 | (c & 63));
      }
    }
    return s;
  }
  template <class T> Json number(P p) {
    T v;
    memcpy(&v, p, sizeof(v));
    if constexpr (std::is_integral_v<T> && sizeof(T) == 8)
      return std::to_string(v);
    return v;
  }
  Json value(C t, P p) {
    int k = a.type_get_type(t);
    switch (k) {
    case 1:
      return nullptr;
    case 2:
      return number<bool>(p);
    case 3:
      return number<uint16_t>(p);
    case 4:
      return number<int8_t>(p);
    case 5:
      return number<uint8_t>(p);
    case 6:
      return number<int16_t>(p);
    case 7:
      return number<uint16_t>(p);
    case 8:
      return number<int32_t>(p);
    case 9:
      return number<uint32_t>(p);
    case 10:
      return number<int64_t>(p);
    case 11:
      return number<uint64_t>(p);
    case 12:
      return number<float>(p);
    case 13:
      return number<double>(p);
    case 24:
      return number<intptr_t>(p);
    case 25:
      return number<uintptr_t>(p);
    default:
      break;
    }
    if (k == 15 || k == 27) {
      P pointer = nullptr;
      memcpy(&pointer, p, sizeof(pointer));
      return {{"pointer", address(pointer)}};
    }
    if (k != 14 && k != 17 && k != 18 && k != 20 && k != 21 && k != 28 &&
        k != 29)
      fail("Unsupported runtime value type");
    auto c = a.class_from_type(t);
    if (c && a.class_is_valuetype(c)) {
      uint32_t align = 0;
      auto size = a.class_value_size(c, &align);
      if (size < 0 || size > 4096)
        fail("Value type exceeds bound");
      const char *hex = "0123456789abcdef";
      std::string b;
      auto bytes = static_cast<unsigned char *>(p);
      for (int i = 0; i < size; ++i) {
        b += hex[bytes[i] >> 4];
        b += hex[bytes[i] & 15];
      }
      return {{"bytes", b}, {"type", typeName(t)}};
    }
    P o = nullptr;
    memcpy(&o, p, sizeof(o));
    if (k == 14)
      return o ? Json(string(o)) : Json(nullptr);
    return hold(o);
  }
  struct Storage {
    alignas(16) std::array<unsigned char, 4096> bytes{};
    P ref = nullptr;
    bool reference = false;
  };
  template <class T> void store(Storage &s, const Json &j) {
    if constexpr (std::is_same_v<T, bool>) {
      if (!j.is_boolean())
        fail("Expected boolean");
    } else if constexpr (std::is_integral_v<T>) {
      if (j.is_string()) {
        auto text = j.get<std::string>();
        T value{};
        auto parsed =
            std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
          fail("Integer out of range or invalid decimal text");
        memcpy(s.bytes.data(), &value, sizeof(value));
        return;
      }
      if (!j.is_number_integer())
        fail("Expected integer");
      const long double n = j.get<long double>();
      if (n < std::numeric_limits<T>::lowest() ||
          n > std::numeric_limits<T>::max())
        fail("Integer out of range");
    } else {
      if (!j.is_number())
        fail("Expected number");
      auto n = j.get<double>();
      if (!std::isfinite(n) || std::abs(n) > std::numeric_limits<T>::max())
        fail("Number out of range");
    }
    auto n = j.get<T>();
    memcpy(s.bytes.data(), &n, sizeof(n));
  }
  Storage input(C t, const Json &j) {
    Storage s;
    auto k = a.type_get_type(t);
    switch (k) {
    case 2:
      store<bool>(s, j);
      return s;
    case 3:
      store<uint16_t>(s, j);
      return s;
    case 4:
      store<int8_t>(s, j);
      return s;
    case 5:
      store<uint8_t>(s, j);
      return s;
    case 6:
      store<int16_t>(s, j);
      return s;
    case 7:
      store<uint16_t>(s, j);
      return s;
    case 8:
      store<int32_t>(s, j);
      return s;
    case 9:
      store<uint32_t>(s, j);
      return s;
    case 10:
      store<int64_t>(s, j);
      return s;
    case 11:
      store<uint64_t>(s, j);
      return s;
    case 12:
      store<float>(s, j);
      return s;
    case 13:
      store<double>(s, j);
      return s;
    case 24:
      store<intptr_t>(s, j);
      return s;
    case 25:
      store<uintptr_t>(s, j);
      return s;
    default:
      break;
    }
    if (k == 15 || k == 27) {
      auto text = j.at("pointer").get<std::string>();
      size_t end = 0;
      if (text.empty() || text[0] == '-')
        fail("Invalid pointer value");
      auto pointer = std::stoull(text, &end, 16);
      if (end != text.size() || pointer > UINTPTR_MAX)
        fail("Invalid pointer value");
      memcpy(s.bytes.data(), &pointer, sizeof(pointer));
      return s;
    }
    if (k != 14 && k != 17 && k != 18 && k != 20 && k != 21 && k != 28 &&
        k != 29)
      fail("Unsupported runtime input type");
    auto c = a.class_from_type(t);
    if (c && a.class_is_valuetype(c)) {
      uint32_t align = 0;
      int size = a.class_value_size(c, &align);
      auto hex = j.at("bytes").get<std::string>();
      if (size < 0 || size > 4096 || hex.size() != size_t(size) * 2 ||
          hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        fail("Value type needs exact-size hex bytes");
      for (int i = 0; i < size; ++i)
        s.bytes[i] = std::stoul(hex.substr(i * 2, 2), nullptr, 16);
      return s;
    }
    s.reference = true;
    if (j.is_null())
      s.ref = nullptr;
    else if (k == 14) {
      auto text = j.get<std::string>();
      if (text.find('\0') != std::string::npos)
        fail("Embedded NUL string unsupported");
      s.ref = a.string_new(text.c_str());
    } else {
      s.ref = object(j.at("object"));
      if (s.ref && c &&
          !a.class_is_assignable_from(c, a.object_get_class(s.ref)))
        fail("Object has incompatible type");
    }
    return s;
  }
  void validateInstance(P o, P c) {
    if (!o || !a.class_is_assignable_from(c, a.object_get_class(o)))
      fail("Select a compatible object instance");
  }
  Json execute(const Json &q) {
    const auto op = q.at("op").get<std::string>();
    if (op == "ping")
      return {{"kind", "ping"}};
    if (op == "readMemory" || op == "writeMemory") {
      auto text = q.at("address").get<std::string>();
      size_t end = 0;
      if (text.empty() || text[0] == '-')
        fail("Invalid memory address");
      auto base = std::stoull(text, &end, 16);
      if (end != text.size() || !base)
        fail("Invalid memory address");
      std::string hex = q.value("bytes", std::string());
      auto count =
          op == "writeMemory" ? hex.size() / 2 : q.value("size", size_t(64));
      if (!count || count > 1024 || base > UINTPTR_MAX - count)
        fail("Memory operations require 1..1024 bytes");
      if (op == "writeMemory" &&
          (hex.size() % 2 || hex.find_first_not_of("0123456789abcdefABCDEF") !=
                                 std::string::npos))
        fail("Invalid hex bytes");
      std::ifstream maps("/proc/self/maps");
      std::string line;
      bool allowed = false;
      while (std::getline(maps, line)) {
        std::istringstream fields(line);
        std::string range, perms;
        fields >> range >> perms;
        auto dash = range.find('-');
        if (dash == std::string::npos)
          continue;
        auto begin = std::stoull(range.substr(0, dash), nullptr, 16),
             finish = std::stoull(range.substr(dash + 1), nullptr, 16);
        if (base >= begin && base + count <= finish && perms[0] == 'r' &&
            (op != "writeMemory" || perms[1] == 'w')) {
          allowed = true;
          break;
        }
      }
      if (!allowed)
        fail("Memory range is not readable/writable in one mapping");
      int fd = open("/proc/self/mem",
                    (op == "writeMemory" ? O_RDWR : O_RDONLY) | O_CLOEXEC);
      if (fd < 0)
        fail("Cannot open target memory");
      std::array<unsigned char, 1024> bytes{};
      ssize_t n;
      if (op == "writeMemory") {
        for (size_t i = 0; i < count; ++i)
          bytes[i] = std::stoul(hex.substr(i * 2, 2), nullptr, 16);
        n = pwrite(fd, bytes.data(), count, base);
      } else
        n = pread(fd, bytes.data(), count, base);
      close(fd);
      if (n != static_cast<ssize_t>(count))
        fail(
            "Memory operation incomplete; a write may have partially executed");
      std::string output;
      const char *digits = "0123456789abcdef";
      for (size_t i = 0; i < count; ++i) {
        output += digits[bytes[i] >> 4];
        output += digits[bytes[i] & 15];
      }
      return {{"kind", "memory"},
              {"address", text},
              {"bytes", output},
              {"written", op == "writeMemory"}};
    }
    if (op == "assemblies" || op == "connect") {
      Json rows = Json::array();
      size_t n = 0;
      auto all = a.domain_get_assemblies(a.domain_get(), &n);
      if (n > 8192)
        fail("Too many assemblies");
      for (size_t i = 0; i < n; ++i) {
        auto image = a.assembly_get_image(all[i]);
        auto id = address(image);
        images[id] = image;
        rows.push_back({{"id", id},
                        {"name", a.image_get_name(image)},
                        {"count", a.image_get_class_count(image)}});
      }
      return {{"rows", rows}, {"kind", "assemblies"}};
    }
    if (op == "classes") {
      auto image = lookup(images, q, "image");
      auto n = a.image_get_class_count(image);
      auto skip = q.value("offset", size_t(0));
      if (skip > n || n > 1000000 || classes.size() > 65536)
        fail("Class index or retained class limit exceeded");
      Json rows = Json::array();
      std::string filter = q.value("filter", std::string());
      for (size_t i = skip; i < n && i < skip + 2048; ++i) {
        auto c = a.image_get_class(image, i);
        if (!c)
          continue;
        auto name = className(c);
        if (name.find(filter) == std::string::npos)
          continue;
        auto id = address(c);
        classes[id] = c;
        rows.push_back({{"id", id}, {"name", name}});
      }
      return {{"kind", "classes"},
              {"rows", rows},
              {"next", std::min(n, skip + 2048)},
              {"total", n}};
    }
    if (op == "members") {
      auto c = lookup(classes, q, "class");
      if (fields.size() + methods.size() > 65536)
        fail("Member cache limit reached; reconnect");
      Json fs = Json::array(), ms = Json::array();
      size_t count = 0;
      for (auto current = c; current && count < 4096;
           current = a.class_get_parent(current)) {
        P it = nullptr;
        while (auto f = a.class_get_fields(current, &it)) {
          if (++count > 4096)
            break;
          fields[address(f)] = f;
          fs.push_back({{"id", address(f)},
                        {"name", a.field_get_name(f)},
                        {"type", typeName(a.field_get_type(f))},
                        {"offset", a.field_get_offset(f)},
                        {"flags", a.field_get_flags(f)},
                        {"owner", className(current)}});
        }
        it = nullptr;
        while (auto m = a.class_get_methods(current, &it)) {
          if (++count > 4096)
            break;
          methods[address(m)] = m;
          Json params = Json::array();
          auto n = a.method_get_param_count(m);
          if (n > 256)
            continue;
          for (uint32_t i = 0; i < n; ++i) {
            auto name = a.method_get_param_name(m, i);
            params.push_back(
                {{"name", name ? name : ""},
                 {"type", typeName(a.method_get_param(m, i))},
                 {"byref", a.type_is_byref(a.method_get_param(m, i))}});
          }
          uint32_t flags = 0;
          P entry = nullptr;
          memcpy(&entry, m, sizeof(entry));
          ms.push_back({{"id", address(m)},
                        {"name", a.method_get_name(m)},
                        {"return", typeName(a.method_get_return_type(m))},
                        {"params", params},
                        {"flags", a.method_get_flags(m, &flags)},
                        {"entry", address(entry)},
                        {"owner", className(current)}});
        }
      }
      return {{"kind", "members"},
              {"fields", fs},
              {"methods", ms},
              {"partial", count >= 4096}};
    }
    if (op == "objects" || op == "components") {
      P array = nullptr;
      if (op == "objects") {
        auto c = lookup(classes, q, "class");
        auto base = findClass("UnityEngine", "Object");
        if (!a.class_is_assignable_from(base, c))
          fail("Scene enumeration requires a UnityEngine.Object subclass");
        bool inactive = q.value("inactive", false);
        auto type = a.type_get_object(a.class_get_type(c));
        array = invoke(method(base, "FindObjectsOfType", 2), nullptr,
                       {type, &inactive});
      } else {
        auto o = object(q.at("object"));
        auto gc = findClass("UnityEngine", "GameObject");
        validateInstance(o, gc);
        auto type = a.type_get_object(
            a.class_get_type(findClass("UnityEngine", "Component")));
        array = invoke(methodWithType(gc, "GetComponents", "System.Type"), o,
                       {type});
      }
      Json rows = Json::array();
      auto n = array ? a.array_length(array) : 0;
      auto objectClass = findClass("UnityEngine", "Object");
      auto getName = method(objectClass, "get_name", 0);
      for (size_t i = 0; i < std::min<uintptr_t>(n, 256); ++i) {
        P o = nullptr;
        memcpy(&o, static_cast<char *>(array) + sizeof(P) * 4 + i * sizeof(P),
               sizeof(P));
        if (!o)
          continue;
        auto row = hold(o);
        try {
          row["name"] = string(invoke(getName, o));
        } catch (...) {
          row["name"] = "<unavailable>";
        }
        rows.push_back(row);
      }
      return {{"kind", "objects"},
              {"rows", rows},
              {"total", n},
              {"partial", n > 256}};
    }
    if (op == "read" || op == "write") {
      auto f = lookup(fields, q, "field");
      auto flags = a.field_get_flags(f);
      auto c = a.field_get_parent(f);
      auto t = a.field_get_type(f);
      P o = nullptr;
      if (flags & 16)
        a.runtime_class_init(c);
      else {
        o = object(q.value("object", Json(nullptr)));
        validateInstance(o, c);
      }
      if (op == "write") {
        if (flags & (32 | 64))
          fail("Readonly and literal fields cannot be written");
        auto v = input(t, q.at("value"));
        P data = v.reference ? static_cast<P>(&v.ref) : v.bytes.data();
        if (flags & 16)
          a.field_static_set_value(f, data);
        else
          a.field_set_value(o, f, data);
      }
      Storage s;
      auto tc = a.class_from_type(t);
      if (tc && a.class_is_valuetype(tc)) {
        uint32_t align = 0;
        if (a.class_value_size(tc, &align) > 4096)
          fail("Field too large");
      }
      if (flags & 16)
        a.field_static_get_value(f, s.bytes.data());
      else
        a.field_get_value(o, f, s.bytes.data());
      return {{"kind", "value"},
              {"field", address(f)},
              {"value", value(t, s.bytes.data())}};
    }
    if (op == "invoke") {
      auto m = lookup(methods, q, "method");
      if (a.method_is_generic(m) && !a.method_is_inflated(m))
        fail("Open generic method cannot be invoked");
      uint32_t impl = 0;
      auto flags = a.method_get_flags(m, &impl);
      P o = nullptr;
      if (!(flags & 16)) {
        o = object(q.value("object", Json(nullptr)));
        validateInstance(o, a.method_get_class(m));
      }
      auto args = q.value("args", Json::array());
      auto n = a.method_get_param_count(m);
      if (!args.is_array() || args.size() != n || n > 32)
        fail("Argument count mismatch or exceeds 32");
      std::vector<Storage> storage;
      storage.reserve(n);
      std::vector<P> pointers;
      std::vector<uint32_t> roots;
      try {
        for (uint32_t i = 0; i < n; ++i) {
          auto t = a.method_get_param(m, i);
          storage.push_back(input(t, args[i]));
          auto &s = storage.back();
          if (s.reference && s.ref)
            roots.push_back(a.gchandle_new(s.ref, true));
          pointers.push_back(
              s.reference
                  ? (a.type_is_byref(t) ? static_cast<P>(&s.ref) : s.ref)
                  : s.bytes.data());
        }
        auto result = invoke(m,
                             o && a.class_is_valuetype(a.method_get_class(m))
                                 ? a.object_unbox(o)
                                 : o,
                             pointers);
        auto rt = a.method_get_return_type(m);
        Json v = nullptr;
        if (result) {
          auto c = a.class_from_type(rt);
          v = c && a.class_is_valuetype(c) ? value(rt, a.object_unbox(result))
                                           : value(rt, &result);
        }
        Json outs = Json::object();
        for (uint32_t i = 0; i < n; ++i)
          if (a.type_is_byref(a.method_get_param(m, i))) {
            auto &s = storage[i];
            outs[std::to_string(i)] =
                value(a.method_get_param(m, i),
                      s.reference ? static_cast<P>(&s.ref) : s.bytes.data());
          }
        for (auto h : roots)
          a.gchandle_free(h);
        return {{"kind", "result"}, {"value", v}, {"out", outs}};
      } catch (...) {
        for (auto h : roots)
          a.gchandle_free(h);
        throw;
      }
    }
    if (op == "release") {
      auto id = q.at("object").get<std::string>();
      auto it = objects.find(id);
      if (it != objects.end()) {
        a.gchandle_free(it->second);
        objects.erase(it);
      }
      return {{"released", id}};
    }
    if (op == "hooks") {
      Json rows = Json::array();
      for (auto &[id, h] : hooks)
        rows.push_back(
            {{"id", id},
             {"calls", h->typed ? h->typed->calls.load() : h->calls.load()},
             {"replacement", bool(h->typed)}});
      return {
          {"kind", "hooks"}, {"rows", rows}, {"retiring", retiredHooks.size()}};
    }
    if (op == "unhook") {
      auto id = q.at("method").get<std::string>();
      auto it = hooks.find(id);
      if (it != hooks.end()) {
        removeHook(it->second);
        hooks.erase(it);
      }
      return {{"unhooked", id}, {"retiring", retiredHooks.size()}};
    }
    if (op == "hook") {
      auto m = lookup(methods, q, "method");
      auto id = address(m);
      if (hooks.count(id))
        fail("Method already hooked; remove it first");
      if (hooks.size() >= 32 || retiredHooks.size() >= 32)
        fail("Hook / in-flight retirement limit 32 reached");
      if (a.method_is_generic(m) && !a.method_is_inflated(m))
        fail("Open generic method cannot be hooked");
      auto holder = std::make_unique<Hook>();
      auto h = holder.get();
      h->freeRoot = a.gchandle_free;
      memcpy(&h->entry, m, sizeof(P));
      if (!h->entry)
        fail("Method has no native entry");
      auto overrides = q.value("arguments", Json::object());
      if (!overrides.is_object())
        fail("Hook arguments must be an index/value object");
      const bool skip = q.value("skip", false);
      if (overrides.empty() && !q.contains("return") && !skip) {
        h->listener = gum_make_call_listener(
            enter, nullptr, h, [](P p) { delete static_cast<Hook *>(p); });
        holder.release();
        if (gum_interceptor_attach(interceptor, h->entry, h->listener, nullptr,
                                   GUM_ATTACH_FLAGS_NONE) != GUM_ATTACH_OK) {
          g_object_unref(h->listener);
          fail("Cannot attach native method hook");
        }
      } else {
        uint32_t impl = 0;
        bool instance = !(a.method_get_flags(m, &impl) & 16);
        auto n = a.method_get_param_count(m);
        if (n > 32)
          fail("Hook parameter limit 32");
        std::vector<std::shared_ptr<NativeType>> types;
        if (instance)
          types.push_back(std::make_shared<NativeType>(&ffi_type_pointer));
        for (uint32_t i = 0; i < n; ++i)
          types.push_back(nativeType(a.method_get_param(m, i)));
        types.push_back(std::make_shared<NativeType>(&ffi_type_pointer));
        auto resultType = nativeType(a.method_get_return_type(m));
        auto encode = [&](C type, const Json &value, size_t size) {
          if (a.type_is_byref(type))
            fail("Hook overrides of ref/out slots are unsupported; use typed "
                 "invocation or memory view");
          auto v = input(type, value);
          std::vector<uint8_t> bytes(size);
          if (size > v.bytes.size())
            fail("Hook value exceeds 4096 bytes");
          if (v.reference) {
            if (size != sizeof(P))
              fail("Hook reference size mismatch");
            if (v.ref) {
              auto root = a.gchandle_new(v.ref, true);
              if (!root)
                fail("Cannot retain Hook value");
              h->roots.push_back(root);
            }
            memcpy(bytes.data(), &v.ref, sizeof(P));
          } else
            memcpy(bytes.data(), v.bytes.data(), size);
          return bytes;
        };
        std::map<unsigned, std::vector<uint8_t>> values;
        for (auto it = overrides.begin(); it != overrides.end(); ++it) {
          size_t end = 0;
          auto index = std::stoul(it.key(), &end);
          if (end != it.key().size() || index >= n)
            fail("Invalid Hook parameter index");
          values[index + instance] =
              encode(a.method_get_param(m, index), it.value(),
                     types[index + instance]->type->size);
        }
        std::vector<uint8_t> returned;
        if (q.contains("return"))
          returned = encode(a.method_get_return_type(m), q["return"],
                            resultType->type->size);
        h->typed = std::make_unique<NativeHook>(
            resultType, std::move(types), std::move(values),
            std::move(returned), q.contains("return"), skip);
        gum_interceptor_begin_transaction(interceptor);
        auto status =
            gum_interceptor_replace(interceptor, h->entry, h->typed->code,
                                    nullptr, &h->typed->original);
        gum_interceptor_end_transaction(interceptor);
        if (status != GUM_REPLACE_OK)
          fail("Cannot replace native method entry");
        holder.release();
      }
      try {
        hooks.emplace(id, h);
      } catch (...) {
        removeHook(h);
        throw;
      }
      return {{"hooked", id},
              {"entry", address(h->entry)},
              {"note", "Shared generic code can affect multiple methods; "
                       "skip=true bypasses the original method"}};
    }
    fail("Unknown runtime operation");
  }
};
Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;
Json Runtime::request(const Json &q) {
  const auto op = q.at("op").get<std::string>();
  if (op == "objects" || op == "components" || op == "read" || op == "write" ||
      op == "invoke")
    return impl_->onMain([&] { return impl_->execute(q); });
  return impl_->execute(q);
}
}
