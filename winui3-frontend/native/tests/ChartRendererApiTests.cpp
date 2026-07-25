#include "../src/chart/ChartRenderer.h"

#include <windows.h>

#include <cassert>
#include <cstdio>
#include <limits>

struct __declspec(uuid("FC084699-67D8-40E1-ADE7-08901D84FFDA"))
CompositorSwapChainInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateGraphicsDevice(
        IUnknown* renderingDevice, IUnknown** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCompositionSurfaceForHandle(
        HANDLE swapChain, IUnknown** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCompositionSurfaceForSwapChain(
        IUnknown* swapChain, IUnknown** result) = 0;
};

class TestCompositor final : public CompositorSwapChainInterop {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (iid == __uuidof(IUnknown) ||
            iid == __uuidof(CompositorSwapChainInterop)) {
            *object = static_cast<CompositorSwapChainInterop*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&references_);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return InterlockedDecrement(&references_);
    }

    HRESULT STDMETHODCALLTYPE CreateGraphicsDevice(
        IUnknown*, IUnknown**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE CreateCompositionSurfaceForHandle(
        HANDLE, IUnknown**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE CreateCompositionSurfaceForSwapChain(
        IUnknown* swapChain, IUnknown** result) override {
        if (!swapChain || !result) {
            return E_POINTER;
        }
        attachedSwapChain_ = swapChain;
        *result = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }

    bool hasSwapChain() const {
        return attachedSwapChain_ != nullptr;
    }

private:
    volatile long references_{1};
    IUnknown* attachedSwapChain_{};
};

int main() {
    auto* chart = tnr_chart_create();
    assert(chart != nullptr);
    assert(tnr_chart_attach(chart, nullptr, nullptr) == 0);

    TestCompositor compositor;
    IUnknown* surface{};
    if (tnr_chart_attach(chart, &compositor, reinterpret_cast<void**>(&surface)) == 0) {
        char error[1024]{};
        tnr_chart_copy_last_error(chart, error, sizeof(error));
        std::fprintf(stderr, "Chart attachment failed: %s\n", error);
        return 1;
    }
    assert(compositor.hasSwapChain());
    assert(surface != nullptr);
    surface->Release();
    assert(tnr_chart_set_x_range(chart, 0, 30) == 1);

    const auto series = tnr_chart_add_series(
        chart, .2f, .7f, .3f, 1, 2, 4, 10);
    assert(series != 0);
    assert(tnr_chart_set_series_y_range(chart, series, 0, 100) == 1);

    const tnr_chart_point initial[] = {
        {0, 1}, {1, 2}, {1, 3}, {2, 4},
    };
    assert(tnr_chart_replace_points(chart, series, initial, 4) == 1);

    const tnr_chart_point appended[] = {
        {3, std::numeric_limits<double>::quiet_NaN()},
        {4, 5},
    };
    assert(tnr_chart_append_points(chart, series, appended, 2) == 1);

    const tnr_chart_point outOfOrder[] = {{3, 6}};
    assert(tnr_chart_append_points(chart, series, outOfOrder, 1) == 0);
    assert(tnr_chart_copy_last_error(chart, nullptr, 0) > 0);

    assert(tnr_chart_clear_series(chart, series) == 1);
    assert(tnr_chart_remove_series(chart, series) == 1);
    assert(tnr_chart_remove_series(chart, series) == 0);
    tnr_chart_destroy(chart);
}
