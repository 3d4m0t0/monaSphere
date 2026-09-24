#include "video/cuda_minimal.hpp"

#include "common.hpp"

#include <cstdio>
#include <dlfcn.h>

namespace vrp::cuda {
namespace {

template <typename T>
bool load_sym(void* lib, T& fn, const char* name) {
  fn = reinterpret_cast<T>(dlsym(lib, name));
  return fn != nullptr;
}

}  // namespace

Api& api() {
  static Api a = [] {
    Api x;
    x.lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!x.lib) x.lib = dlopen("libcuda.so", RTLD_NOW | RTLD_LOCAL);
    if (!x.lib) return x;
    bool ok = true;
    ok &= load_sym(x.lib, x.cuInit, "cuInit");
    ok &= load_sym(x.lib, x.cuDeviceGet, "cuDeviceGet");
    ok &= load_sym(x.lib, x.cuDeviceGetCount, "cuDeviceGetCount");
    ok &= load_sym(x.lib, x.cuCtxGetCurrent, "cuCtxGetCurrent");
    ok &= load_sym(x.lib, x.cuCtxPushCurrent, "cuCtxPushCurrent");
    ok &= load_sym(x.lib, x.cuCtxPopCurrent, "cuCtxPopCurrent");
    load_sym(x.lib, x.cuCtxSynchronize, "cuCtxSynchronize");
    if (!load_sym(x.lib, x.cuMemFree, "cuMemFree_v2")) load_sym(x.lib, x.cuMemFree, "cuMemFree");
    ok &= load_sym(x.lib, x.cuImportExternalMemory, "cuImportExternalMemory");
    ok &= load_sym(x.lib, x.cuExternalMemoryGetMappedBuffer, "cuExternalMemoryGetMappedBuffer");
    ok &= load_sym(x.lib, x.cuDestroyExternalMemory, "cuDestroyExternalMemory");
    // Prefer _v2: unversioned cuMemcpy2D is the legacy v1 ABI (different struct).
    if (!load_sym(x.lib, x.cuMemcpy2D, "cuMemcpy2D_v2")) ok &= load_sym(x.lib, x.cuMemcpy2D, "cuMemcpy2D");
    if (!load_sym(x.lib, x.cuMemcpy2DAsync, "cuMemcpy2DAsync_v2"))
      ok &= load_sym(x.lib, x.cuMemcpy2DAsync, "cuMemcpy2DAsync");
    load_sym(x.lib, x.cuMemcpyDtoD, "cuMemcpyDtoD_v2");
    if (!x.cuMemcpyDtoD) load_sym(x.lib, x.cuMemcpyDtoD, "cuMemcpyDtoD");
    load_sym(x.lib, x.cuMemcpyDtoDAsync, "cuMemcpyDtoDAsync_v2");
    if (!x.cuMemcpyDtoDAsync) load_sym(x.lib, x.cuMemcpyDtoDAsync, "cuMemcpyDtoDAsync");
    ok &= load_sym(x.lib, x.cuStreamSynchronize, "cuStreamSynchronize");
    load_sym(x.lib, x.cuPointerGetAttribute, "cuPointerGetAttribute");
    load_sym(x.lib, x.cuGetErrorName, "cuGetErrorName");
    load_sym(x.lib, x.cuGetErrorString, "cuGetErrorString");
    load_sym(x.lib, x.cuDeviceGetUuid, "cuDeviceGetUuid");
    load_sym(x.lib, x.cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain");
    load_sym(x.lib, x.cuDevicePrimaryCtxRelease, "cuDevicePrimaryCtxRelease");
    if (!ok) {
      dlclose(x.lib);
      x = {};
      return x;
    }
    if (x.cuInit(0) != CUDA_SUCCESS) {
      dlclose(x.lib);
      x = {};
      return x;
    }
    x.ok = true;
    VRP_LOG("CUDA driver API loaded (zero-copy interop available)");
    return x;
  }();
  return a;
}

bool available() { return api().ok; }

const char* err_name(CUresult r) {
  auto& a = api();
  const char* name = nullptr;
  if (a.cuGetErrorName && a.cuGetErrorName(r, &name) == CUDA_SUCCESS && name) return name;
  static thread_local char buf[32];
  std::snprintf(buf, sizeof(buf), "CUresult(%d)", static_cast<int>(r));
  return buf;
}

}  // namespace vrp::cuda
