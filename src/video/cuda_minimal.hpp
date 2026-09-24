#pragma once

#include <cstddef>
#include <cstdint>

// Minimal CUDA Driver API surface (dynamic load of libcuda.so.1).
// Avoids requiring the CUDA Toolkit headers.

namespace vrp::cuda {

using CUdeviceptr = unsigned long long;
using CUdevice = int;
using CUcontext = struct CUctx_st*;
using CUstream = struct CUstream_st*;
using CUexternalMemory = struct CUextMemory_st*;
using CUresult = int;

constexpr CUresult CUDA_SUCCESS = 0;

enum CUexternalMemoryHandleType {
  CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD = 1,
};

// cuda.h: flags must be 0 or CUDA_EXTERNAL_MEMORY_DEDICATED when memory is dedicated.
constexpr unsigned int CUDA_EXTERNAL_MEMORY_DEDICATED = 0x1u;

enum CUmemorytype {
  CU_MEMORYTYPE_HOST = 1,
  CU_MEMORYTYPE_DEVICE = 2,
  CU_MEMORYTYPE_ARRAY = 3,
  CU_MEMORYTYPE_UNIFIED = 4,
};

// Must match cuda.h CUDA_EXTERNAL_MEMORY_HANDLE_DESC_v1 (handle union is 16 bytes → sizeof 104).
struct CUDA_EXTERNAL_MEMORY_HANDLE_DESC {
  CUexternalMemoryHandleType type;
  union {
    int fd;
    struct {
      void* handle;
      const void* name;
    } win32;
    const void* nvSciBufObject;
  } handle;
  unsigned long long size;
  unsigned int flags;
  unsigned int reserved[16];
};

struct CUDA_EXTERNAL_MEMORY_BUFFER_DESC {
  unsigned long long offset;
  unsigned long long size;
  unsigned int flags;
  unsigned int reserved[16];
};

struct CUDA_MEMCPY2D {
  size_t srcXInBytes;
  size_t srcY;
  CUmemorytype srcMemoryType;
  const void* srcHost;
  CUdeviceptr srcDevice;
  void* srcArray;
  size_t srcPitch;
  size_t dstXInBytes;
  size_t dstY;
  CUmemorytype dstMemoryType;
  void* dstHost;
  CUdeviceptr dstDevice;
  void* dstArray;
  size_t dstPitch;
  size_t WidthInBytes;
  size_t Height;
};
static_assert(sizeof(CUDA_MEMCPY2D) == 128, "CUDA_MEMCPY2D must match Driver API v2");
static_assert(sizeof(CUDA_EXTERNAL_MEMORY_HANDLE_DESC) == 104,
              "CUDA_EXTERNAL_MEMORY_HANDLE_DESC must match Driver API");

struct Api {
  bool ok = false;
  void* lib = nullptr;

  CUresult (*cuInit)(unsigned int) = nullptr;
  CUresult (*cuDeviceGet)(CUdevice*, int) = nullptr;
  CUresult (*cuDeviceGetCount)(int*) = nullptr;
  CUresult (*cuDeviceGetUuid)(void* uuid /*CUuuid*/, CUdevice) = nullptr;  // optional
  CUresult (*cuDevicePrimaryCtxRetain)(CUcontext*, CUdevice) = nullptr;
  CUresult (*cuDevicePrimaryCtxRelease)(CUdevice) = nullptr;
  CUresult (*cuCtxGetCurrent)(CUcontext*) = nullptr;
  CUresult (*cuCtxPushCurrent)(CUcontext) = nullptr;
  CUresult (*cuCtxPopCurrent)(CUcontext*) = nullptr;
  CUresult (*cuCtxSynchronize)() = nullptr;
  CUresult (*cuMemFree)(CUdeviceptr) = nullptr;
  CUresult (*cuImportExternalMemory)(CUexternalMemory*, const CUDA_EXTERNAL_MEMORY_HANDLE_DESC*) = nullptr;
  CUresult (*cuExternalMemoryGetMappedBuffer)(CUdeviceptr*, CUexternalMemory,
                                              const CUDA_EXTERNAL_MEMORY_BUFFER_DESC*) = nullptr;
  CUresult (*cuDestroyExternalMemory)(CUexternalMemory) = nullptr;
  CUresult (*cuMemcpy2D)(const CUDA_MEMCPY2D*) = nullptr;           // binds to cuMemcpy2D_v2
  CUresult (*cuMemcpy2DAsync)(const CUDA_MEMCPY2D*, CUstream) = nullptr;
  CUresult (*cuMemcpyDtoD)(CUdeviceptr, CUdeviceptr, size_t) = nullptr;
  CUresult (*cuMemcpyDtoDAsync)(CUdeviceptr, CUdeviceptr, size_t, CUstream) = nullptr;
  CUresult (*cuStreamSynchronize)(CUstream) = nullptr;
  CUresult (*cuPointerGetAttribute)(void*, unsigned int, CUdeviceptr) = nullptr;
  CUresult (*cuGetErrorName)(CUresult, const char**) = nullptr;
  CUresult (*cuGetErrorString)(CUresult, const char**) = nullptr;
};

Api& api();
bool available();
const char* err_name(CUresult r);

}  // namespace vrp::cuda
