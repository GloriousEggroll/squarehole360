// Windows only: every DxcCreateInstance call in this executable creates the
// object through DxcCreateInstance2 with a mimalloc-backed IMalloc.
//
// Without an IMalloc, DXC allocates through the COM task allocator, which is
// HeapAlloc. Under Wine that heap serializes the shader compiler's many small
// allocations: when a new area brings in shaders, up to a third of the
// compile threads' time went to RtlAllocateHeap. Compilation output is
// unchanged; only where DXC's working memory comes from differs.
//
// Callers include dxcapi.h, whose dllimport declaration calls through
// __imp_DxcCreateInstance. Defining that pointer in an object linked into the
// executable takes precedence over the import library's member for it, so the
// callers themselves stay untouched.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>

#include <dxcapi.h>
#include <mimalloc.h>

namespace {
class MimallocMalloc final : public IMalloc {
  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out)
            return E_POINTER;
        if (id == __uuidof(IUnknown) || id == __uuidof(IMalloc)) {
            *out = static_cast<IMalloc *>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    // A process-lifetime singleton; DXC objects may outlive any scope.
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    void *STDMETHODCALLTYPE Alloc(SIZE_T bytes) override { return mi_malloc(bytes); }
    void *STDMETHODCALLTYPE Realloc(void *pointer, SIZE_T bytes) override { return mi_realloc(pointer, bytes); }
    void STDMETHODCALLTYPE Free(void *pointer) override { mi_free(pointer); }
    SIZE_T STDMETHODCALLTYPE GetSize(void *pointer) override { return pointer ? mi_usable_size(pointer) : SIZE_T(-1); }
    int STDMETHODCALLTYPE DidAlloc(void *pointer) override { return pointer ? (mi_is_in_heap_region(pointer) ? 1 : 0) : -1; }
    void STDMETHODCALLTYPE HeapMinimize() override { mi_collect(false); }
};
MimallocMalloc dxc_malloc;

HRESULT __stdcall CreateWithMimalloc(REFCLSID clsid, REFIID iid, LPVOID *out) {
    return DxcCreateInstance2(&dxc_malloc, clsid, iid, out);
}
} // namespace

extern "C" decltype(&CreateWithMimalloc) __imp_DxcCreateInstance = &CreateWithMimalloc;
#endif
