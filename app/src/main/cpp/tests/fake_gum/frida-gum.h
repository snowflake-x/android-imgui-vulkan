#pragma once
#include <cstdint>
using gpointer = void *;
using GumAddress = uintptr_t;
struct FakeObject {
  virtual ~FakeObject() = default;
};
struct GumModule : FakeObject {};
struct GumInterceptor : FakeObject {};
struct GumInvocationContext {
  void *args[32]{};
  void *result = nullptr;
};
using GumInvocationCallback = void (*)(GumInvocationContext *, gpointer);
struct GumInvocationListener : FakeObject {
  void *data;
  void (*destroy)(void *);
  ~GumInvocationListener() { destroy(data); }
};
constexpr int GUM_ATTACH_FLAGS_NONE = 0, GUM_ATTACH_OK = 0;
GumModule *gum_process_find_module_by_name(const char *);
GumAddress gum_module_find_export_by_name(GumModule *, const char *);
inline void g_object_unref(FakeObject *o) { delete o; }
inline GumInterceptor *gum_interceptor_obtain() { return new GumInterceptor; }
inline void gum_interceptor_detach(GumInterceptor *, GumInvocationListener *) {}
inline int gum_interceptor_attach(GumInterceptor *, void *,
                                  GumInvocationListener *, void *, int) {
  return 0;
}
inline GumInvocationListener *gum_make_call_listener(GumInvocationCallback,
                                                     GumInvocationCallback,
                                                     void *data,
                                                     void (*destroy)(void *)) {
  auto l = new GumInvocationListener;
  l->data = data;
  l->destroy = destroy;
  return l;
}
inline void gum_invocation_context_replace_nth_argument(GumInvocationContext *c,
                                                        unsigned n, void *v) {
  c->args[n] = v;
}
inline void gum_invocation_context_replace_return_value(GumInvocationContext *c,
                                                        void *v) {
  c->result = v;
}

constexpr int GUM_REPLACE_OK = 0;
inline int gum_interceptor_replace(GumInterceptor *, void *entry, void *,
                                   void *, void **original) {
  *original = entry;
  return 0;
}
inline void gum_interceptor_revert(GumInterceptor *, void *) {}
inline bool gum_interceptor_flush(GumInterceptor *) { return true; }
inline void gum_interceptor_begin_transaction(GumInterceptor *) {}
inline void gum_interceptor_end_transaction(GumInterceptor *) {}
