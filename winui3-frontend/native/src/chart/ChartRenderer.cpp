#include "ChartRenderer.h"

#include "ChartData.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct __declspec(uuid("FC084699-67D8-40E1-ADE7-08901D84FFDA"))
CompositorSwapChainInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateGraphicsDevice(
        IUnknown* renderingDevice, IUnknown** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCompositionSurfaceForHandle(
        HANDLE swapChain, IUnknown** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCompositionSurfaceForSwapChain(
        IUnknown* swapChain, IUnknown** result) = 0;
};

constexpr char ShaderSource[] = R"(
cbuffer DrawConstants : register(b0)
{
    float2 xRange;
    float2 yRange;
    float2 viewportSize;
    float thickness;
    float unused;
    float4 color;
};

struct VertexInput
{
    float4 segment : INSTANCE_SEGMENT;
    uint vertexId : SV_VertexID;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 linePosition : TEXCOORD0;
    nointerpolation float lineLength : TEXCOORD1;
    nointerpolation float lineRadius : TEXCOORD2;
};

float2 ToClip(float2 value)
{
    float x = ((value.x - xRange.x) / (xRange.y - xRange.x)) * 2.0 - 1.0;
    float y = ((value.y - yRange.x) / (yRange.y - yRange.x)) * 2.0 - 1.0;
    return float2(x, y);
}

PixelInput VSMain(VertexInput input)
{
    float2 p0 = ToClip(input.segment.xy);
    float2 p1 = ToClip(input.segment.zw);
    float2 deltaPixels = (p1 - p0) * float2(viewportSize.x, -viewportSize.y) * 0.5;
    float lengthPixels = max(length(deltaPixels), 0.0001);
    float2 tangentPixels = deltaPixels / lengthPixels;
    float2 normalPixels = float2(-tangentPixels.y, tangentPixels.x);
    float radius = max(thickness * 0.5, 0.25);
    float outerRadius = radius + 1.5;

    static const uint endpoint[6] = {0, 0, 1, 1, 0, 1};
    static const float side[6] = {-1, 1, -1, -1, 1, 1};
    float along = endpoint[input.vertexId] == 0
        ? -outerRadius
        : lengthPixels + outerRadius;
    float across = side[input.vertexId] * outerRadius;
    float2 pixelOffset = tangentPixels * along + normalPixels * across;
    float2 clipOffset =
        pixelOffset * float2(2.0 / viewportSize.x, -2.0 / viewportSize.y);

    PixelInput output;
    output.position = float4(p0 + clipOffset, 0, 1);
    output.linePosition = float2(along, across);
    output.lineLength = lengthPixels;
    output.lineRadius = radius;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    float alongOutside = max(
        max(-input.linePosition.x, input.linePosition.x - input.lineLength),
        0.0);
    float distanceToLine = length(
        float2(alongOutside, input.linePosition.y));
    float antialiasWidth = max(fwidth(distanceToLine), 0.75);
    float coverage = 1.0 - smoothstep(
        input.lineRadius - antialiasWidth,
        input.lineRadius + antialiasWidth,
        distanceToLine);
    float alpha = color.a * coverage;
    return float4(color.rgb * alpha, alpha);
}
)";

struct DrawConstants {
    float xRange[2];
    float yRange[2];
    float viewportSize[2];
    float thickness;
    float unused;
    float color[4];
};

static_assert(sizeof(DrawConstants) % 16 == 0);

struct Color {
    float r{};
    float g{};
    float b{};
    float a{1};
};

struct Series {
    std::vector<tnr::chart::Point> points;
    std::size_t first{};
    std::size_t maximumPoints{};
    double maximumXSpan{};
    double yMinimum{};
    double yMaximum{1};
    Color color{};
    float thickness{1.5f};
    bool visible{true};
    std::uint64_t revision{};
    std::uint64_t cachedRevision{std::numeric_limits<std::uint64_t>::max()};
    double cachedXMinimum{};
    double cachedXMaximum{};
    std::uint32_t cachedWidth{};
    tnr::chart::ReducedSegments cachedSegments;

    std::span<const tnr::chart::Point> activePoints() const {
        return std::span(points).subspan(first);
    }

    void trim() {
        if (points.empty()) {
            first = 0;
            return;
        }
        if (maximumPoints > 0 && points.size() - first > maximumPoints) {
            first = points.size() - maximumPoints;
        }
        if (maximumXSpan > 0 && points.size() - first > 1) {
            const auto cutoff = points.back().x - maximumXSpan;
            const auto begin = points.begin() + static_cast<std::ptrdiff_t>(first);
            const auto found = std::lower_bound(
                begin, points.end(), cutoff,
                [](const tnr::chart::Point& point, const double value) {
                    return point.x < value;
                });
            first = static_cast<std::size_t>(found - points.begin());
        }
        if (first >= 4096 && first * 2 >= points.size()) {
            points.erase(
                points.begin(),
                points.begin() + static_cast<std::ptrdiff_t>(first));
            first = 0;
        }
    }
};

std::string hresultMessage(const char* operation, const HRESULT result) {
    char suffix[32]{};
    std::snprintf(
        suffix, sizeof(suffix), " (HRESULT 0x%08X)",
        static_cast<unsigned int>(result));
    return std::string(operation) + suffix;
}

void check(const HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(hresultMessage(operation, result));
    }
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const auto required = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), required, nullptr, nullptr);
    return result;
}

// D3D11 devices are designed to own resources for many render targets. Keep
// the device, immediate context, and immutable pipeline state process-wide;
// each Renderer below still owns its swap chain and dynamic upload buffers.
class SharedGraphics {
public:
    static SharedGraphics& instance() {
        static SharedGraphics graphics;
        return graphics;
    }

    std::unique_lock<std::recursive_mutex> lock() {
        return std::unique_lock(mutex_);
    }

    void ensure() {
        if (device_) {
            return;
        }
        try {
            create(false);
        } catch (...) {
            release();
            throw;
        }
    }

    void recreate(const bool forceWarp) {
        release();
        try {
            create(forceWarp);
        } catch (...) {
            release();
            throw;
        }
    }

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    ID3D11VertexShader* vertexShader() const { return vertexShader_.Get(); }
    ID3D11PixelShader* pixelShader() const { return pixelShader_.Get(); }
    ID3D11InputLayout* inputLayout() const { return inputLayout_.Get(); }
    ID3D11RasterizerState* rasterizerState() const {
        return rasterizerState_.Get();
    }
    ID3D11BlendState* blendState() const { return blendState_.Get(); }
    std::uint64_t generation() const { return generation_; }
    bool usingWarp() const { return usingWarp_; }
    const std::string& adapterName() const { return adapterName_; }

private:
    void create(const bool forceWarp) {
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        auto driver = forceWarp
            ? D3D_DRIVER_TYPE_WARP
            : D3D_DRIVER_TYPE_HARDWARE;
        auto result = D3D11CreateDevice(
            nullptr,
            driver,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            &device_,
            nullptr,
            &context_);
        if (FAILED(result) && !forceWarp) {
            driver = D3D_DRIVER_TYPE_WARP;
            result = D3D11CreateDevice(
                nullptr,
                driver,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels,
                static_cast<UINT>(std::size(levels)),
                D3D11_SDK_VERSION,
                &device_,
                nullptr,
                &context_);
        }
        check(result, "D3D11CreateDevice");
        usingWarp_ = driver == D3D_DRIVER_TYPE_WARP;

        ComPtr<IDXGIDevice> dxgiDevice;
        check(device_.As(&dxgiDevice), "Query IDXGIDevice");
        ComPtr<IDXGIAdapter> adapter;
        check(dxgiDevice->GetAdapter(&adapter), "GetAdapter");
        DXGI_ADAPTER_DESC description{};
        check(adapter->GetDesc(&description), "Get adapter description");
        adapterName_ = wideToUtf8(description.Description);

        createPipeline();
        ++generation_;
    }

    void createPipeline() {
        ComPtr<ID3DBlob> vertexBlob;
        ComPtr<ID3DBlob> pixelBlob;
        ComPtr<ID3DBlob> errors;
        auto result = D3DCompile(
            ShaderSource,
            sizeof(ShaderSource) - 1,
            "TrackNRace.ChartRenderer",
            nullptr,
            nullptr,
            "VSMain",
            "vs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            &vertexBlob,
            &errors);
        if (FAILED(result)) {
            const auto message = errors
                ? std::string(
                    static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize())
                : hresultMessage("Compile vertex shader", result);
            throw std::runtime_error(message);
        }
        errors.Reset();
        result = D3DCompile(
            ShaderSource,
            sizeof(ShaderSource) - 1,
            "TrackNRace.ChartRenderer",
            nullptr,
            nullptr,
            "PSMain",
            "ps_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            &pixelBlob,
            &errors);
        if (FAILED(result)) {
            const auto message = errors
                ? std::string(
                    static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize())
                : hresultMessage("Compile pixel shader", result);
            throw std::runtime_error(message);
        }

        check(
            device_->CreateVertexShader(
                vertexBlob->GetBufferPointer(),
                vertexBlob->GetBufferSize(),
                nullptr,
                &vertexShader_),
            "CreateVertexShader");
        check(
            device_->CreatePixelShader(
                pixelBlob->GetBufferPointer(),
                pixelBlob->GetBufferSize(),
                nullptr,
                &pixelShader_),
            "CreatePixelShader");

        const D3D11_INPUT_ELEMENT_DESC elements[] = {{
            "INSTANCE_SEGMENT",
            0,
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            0,
            0,
            D3D11_INPUT_PER_INSTANCE_DATA,
            1,
        }};
        check(
            device_->CreateInputLayout(
                elements,
                1,
                vertexBlob->GetBufferPointer(),
                vertexBlob->GetBufferSize(),
                &inputLayout_),
            "CreateInputLayout");

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.ScissorEnable = TRUE;
        rasterizer.DepthClipEnable = TRUE;
        check(
            device_->CreateRasterizerState(
                &rasterizer, &rasterizerState_),
            "CreateRasterizerState");

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        check(
            device_->CreateBlendState(&blend, &blendState_),
            "CreateBlendState");
    }

    void release() {
        blendState_.Reset();
        rasterizerState_.Reset();
        inputLayout_.Reset();
        pixelShader_.Reset();
        vertexShader_.Reset();
        context_.Reset();
        device_.Reset();
        adapterName_.clear();
    }

    std::recursive_mutex mutex_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11RasterizerState> rasterizerState_;
    ComPtr<ID3D11BlendState> blendState_;
    std::uint64_t generation_{};
    bool usingWarp_{};
    std::string adapterName_;
};

class Renderer {
public:
    bool attach(IUnknown* compositor, IUnknown** compositionSurface) {
        if (!compositor || !compositionSurface) {
            return fail("Compositor or composition-surface pointer was null.");
        }
        *compositionSurface = nullptr;
        auto graphicsLock = graphics_.lock();
        if (compositor_.Get() != compositor || !compositionSurface_) {
            compositor_ = compositor;
            if (!recoverDevice(false, false)) {
                return false;
            }
        }
        *compositionSurface = compositionSurface_.Get();
        (*compositionSurface)->AddRef();
        return true;
    }

    bool resize(
        const std::uint32_t width,
        const std::uint32_t height,
        const float scale) {
        width_ = std::max(1u, width);
        height_ = std::max(1u, height);
        scale_ = std::isfinite(scale) && scale > 0 ? scale : 1;
        if (!swapChain_) {
            return true;
        }
        return guard([&] {
            auto graphicsLock = graphics_.lock();
            if (!ensureCurrentDevice()) {
                throw std::runtime_error(lastError_);
            }
            renderTarget_.Reset();
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            check(
                swapChain_->ResizeBuffers(
                    2, width_, height_, DXGI_FORMAT_B8G8R8A8_UNORM, 0),
                "ResizeBuffers");
            configureSwapChainScale();
            createRenderTarget();
        });
    }

    void setBackground(const Color color) {
        background_ = color;
    }

    void setGridColor(const Color color) {
        gridColor_ = color;
    }

    bool setXRange(const double minimum, const double maximum) {
        if (!validRange(minimum, maximum)) {
            return fail("X axis range must be finite and increasing.");
        }
        xMinimum_ = minimum;
        xMaximum_ = maximum;
        return true;
    }

    bool setVerticalGrid(const double* values, const std::size_t count) {
        if (count > 0 && !values) {
            return fail("Grid value pointer was null.");
        }
        verticalGrid_.assign(values, values + count);
        return true;
    }

    bool setHorizontalGrid(const double* values, const std::size_t count) {
        if (count > 0 && !values) {
            return fail("Grid value pointer was null.");
        }
        horizontalGrid_.assign(values, values + count);
        return true;
    }

    std::uint32_t addSeries(
        const Color color,
        const float thickness,
        const std::size_t maximumPoints,
        const double maximumXSpan) {
        const auto id = nextSeriesId_++;
        auto series = std::make_unique<Series>();
        series->color = color;
        series->thickness = std::max(0.5f, thickness);
        series->maximumPoints = maximumPoints;
        series->maximumXSpan = std::max(0.0, maximumXSpan);
        series_.emplace(id, std::move(series));
        seriesOrder_.push_back(id);
        return id;
    }

    bool removeSeries(const std::uint32_t id) {
        if (series_.erase(id) == 0) {
            return fail("Unknown series handle.");
        }
        std::erase(seriesOrder_, id);
        return true;
    }

    bool setSeriesStyle(
        const std::uint32_t id,
        const Color color,
        const float thickness,
        const bool visible) {
        auto* series = findSeries(id);
        if (!series) {
            return false;
        }
        series->color = color;
        series->thickness = std::max(0.5f, thickness);
        series->visible = visible;
        return true;
    }

    bool setSeriesYRange(
        const std::uint32_t id,
        const double minimum,
        const double maximum) {
        auto* series = findSeries(id);
        if (!series) {
            return false;
        }
        if (!validRange(minimum, maximum)) {
            return fail("Y axis range must be finite and increasing.");
        }
        series->yMinimum = minimum;
        series->yMaximum = maximum;
        return true;
    }

    bool replacePoints(
        const std::uint32_t id,
        const tnr_chart_point* points,
        const std::size_t count) {
        auto* series = findSeries(id);
        if (!series || !validatePoints(points, count, nullptr)) {
            return false;
        }
        series->points.clear();
        series->first = 0;
        series->points.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            series->points.push_back({points[index].x, points[index].y});
        }
        series->trim();
        ++series->revision;
        return true;
    }

    bool appendPoints(
        const std::uint32_t id,
        const tnr_chart_point* points,
        const std::size_t count) {
        auto* series = findSeries(id);
        if (!series) {
            return false;
        }
        const double* previous =
            series->points.size() > series->first
                ? &series->points.back().x
                : nullptr;
        if (!validatePoints(points, count, previous)) {
            return false;
        }
        series->points.reserve(series->points.size() + count);
        for (std::size_t index = 0; index < count; ++index) {
            series->points.push_back({points[index].x, points[index].y});
        }
        series->trim();
        ++series->revision;
        return true;
    }

    bool clearSeries(const std::uint32_t id) {
        auto* series = findSeries(id);
        if (!series) {
            return false;
        }
        series->points.clear();
        series->first = 0;
        ++series->revision;
        return true;
    }

    bool render() {
        if (!swapChain_ || !renderTarget_) {
            return true;
        }
        auto graphicsLock = graphics_.lock();
        if (!ensureCurrentDevice()) {
            return false;
        }
        const auto started = std::chrono::steady_clock::now();
        diagnostics_ = {};
        diagnostics_.using_warp = graphics_.usingWarp() ? 1 : 0;
        return guard([&] {
            const float clear[] = {
                background_.r * background_.a,
                background_.g * background_.a,
                background_.b * background_.a,
                background_.a};
            context_->OMSetRenderTargets(1, renderTarget_.GetAddressOf(), nullptr);
            context_->ClearRenderTargetView(renderTarget_.Get(), clear);

            D3D11_VIEWPORT viewport{
                0, 0, static_cast<float>(width_), static_cast<float>(height_),
                0, 1};
            context_->RSSetViewports(1, &viewport);
            D3D11_RECT scissor{
                0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
            context_->RSSetScissorRects(1, &scissor);

            drawGrid();
            for (const auto id : seriesOrder_) {
                const auto& series = series_.at(id);
                if (!series->visible ||
                    !validRange(series->yMinimum, series->yMaximum)) {
                    continue;
                }
                const auto points = series->activePoints();
                if (series->cachedRevision != series->revision ||
                    series->cachedXMinimum != xMinimum_ ||
                    series->cachedXMaximum != xMaximum_ ||
                    series->cachedWidth != width_) {
                    series->cachedSegments = tnr::chart::buildSegments(
                        points, xMinimum_, xMaximum_, width_);
                    series->cachedRevision = series->revision;
                    series->cachedXMinimum = xMinimum_;
                    series->cachedXMaximum = xMaximum_;
                    series->cachedWidth = width_;
                }
                const auto& reduced = series->cachedSegments;
                diagnostics_.source_points += reduced.visiblePointCount;
                diagnostics_.submitted_segments += reduced.segments.size();
                diagnostics_.used_reduction |= reduced.reduced ? 1 : 0;
                drawSegments(
                    reduced.segments,
                    0,
                    xMaximum_ - xMinimum_,
                    series->yMinimum,
                    series->yMaximum,
                    series->color,
                    series->thickness * scale_);
            }

            // This swap chain is consumed by the WinUI compositor, which
            // performs the display synchronization. Waiting for a vertical
            // blank here blocks the UI thread while holding the process-wide
            // D3D context lock and serializes every chart behind that wait.
            // Submit immediately and let the flip queue retain the newest
            // telemetry frame for composition.
            const auto presented = swapChain_->Present(0, 0);
            if (presented == DXGI_ERROR_DEVICE_REMOVED ||
                presented == DXGI_ERROR_DEVICE_RESET) {
                ++deviceLossCount_;
                if (!recoverDevice(deviceLossCount_ > 1, true)) {
                    throw std::runtime_error(lastError_);
                }
            } else {
                check(presented, "Present");
                deviceLossCount_ = 0;
            }
            const auto finished = std::chrono::steady_clock::now();
            diagnostics_.frame_milliseconds =
                std::chrono::duration<double, std::milli>(
                    finished - started).count();
        });
    }

    const tnr_chart_diagnostics& diagnostics() const {
        return diagnostics_;
    }

    const std::string& lastError() const {
        return lastError_;
    }

    std::string adapterName() const {
        auto graphicsLock = graphics_.lock();
        return graphics_.adapterName();
    }

    std::uint64_t surfaceGeneration() const {
        return surfaceGeneration_;
    }

private:
    bool validRange(const double minimum, const double maximum) const {
        return std::isfinite(minimum) && std::isfinite(maximum) &&
            maximum > minimum;
    }

    bool validatePoints(
        const tnr_chart_point* points,
        const std::size_t count,
        const double* previous) {
        if (count > 0 && !points) {
            return fail("Point pointer was null.");
        }
        auto lastX = previous ? *previous : -std::numeric_limits<double>::infinity();
        for (std::size_t index = 0; index < count; ++index) {
            if (!std::isfinite(points[index].x)) {
                return fail("Point X values must be finite.");
            }
            if (points[index].x < lastX) {
                return fail("Point X values must be monotonically nondecreasing.");
            }
            lastX = points[index].x;
        }
        return true;
    }

    Series* findSeries(const std::uint32_t id) {
        const auto found = series_.find(id);
        if (found == series_.end()) {
            fail("Unknown series handle.");
            return nullptr;
        }
        return found->second.get();
    }

    bool fail(std::string message) {
        lastError_ = std::move(message);
        return false;
    }

    template <typename Function>
    bool guard(Function&& function) {
        try {
            function();
            lastError_.clear();
            return true;
        } catch (const std::exception& error) {
            return fail(error.what());
        } catch (...) {
            return fail("Unknown native chart renderer error.");
        }
    }

    bool ensureCurrentDevice() {
        if (graphicsGeneration_ == graphics_.generation()) {
            return true;
        }
        return recoverDevice(false, false);
    }

    bool recoverDevice(
        const bool forceWarp,
        const bool recreateSharedDevice) {
        return guard([&] {
            releaseDeviceResources();
            if (recreateSharedDevice) {
                graphics_.recreate(forceWarp);
            } else {
                graphics_.ensure();
            }
            createDeviceReferences();
            createPerChartPipelineResources();
            createSwapChain();
            graphicsGeneration_ = graphics_.generation();
            ++surfaceGeneration_;
        });
    }

    void releaseDeviceResources() {
        compositionSurface_.Reset();
        segmentBuffer_.Reset();
        constantBuffer_.Reset();
        blendState_.Reset();
        rasterizerState_.Reset();
        inputLayout_.Reset();
        pixelShader_.Reset();
        vertexShader_.Reset();
        renderTarget_.Reset();
        swapChain_.Reset();
        context_.Reset();
        device_.Reset();
        segmentCapacity_ = 0;
    }

    void createDeviceReferences() {
        device_ = graphics_.device();
        context_ = graphics_.context();
        vertexShader_ = graphics_.vertexShader();
        pixelShader_ = graphics_.pixelShader();
        inputLayout_ = graphics_.inputLayout();
        rasterizerState_ = graphics_.rasterizerState();
        blendState_ = graphics_.blendState();
    }

    void createPerChartPipelineResources() {
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = sizeof(DrawConstants);
        constants.Usage = D3D11_USAGE_DYNAMIC;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check(
            device_->CreateBuffer(&constants, nullptr, &constantBuffer_),
            "Create constant buffer");
    }

    void createSwapChain() {
        if (!compositor_) {
            throw std::runtime_error("No compositor is attached.");
        }

        ComPtr<CompositorSwapChainInterop> compositorInterop;
        check(
            compositor_.As(&compositorInterop),
            "Query ICompositorSwapChainInterop");

        ComPtr<IDXGIDevice> dxgiDevice;
        check(device_.As(&dxgiDevice), "Query IDXGIDevice");
        ComPtr<IDXGIAdapter> adapter;
        check(dxgiDevice->GetAdapter(&adapter), "GetAdapter");
        ComPtr<IDXGIFactory2> factory;
        check(adapter->GetParent(IID_PPV_ARGS(&factory)), "Get DXGI factory");

        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width_;
        description.Height = height_;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

        check(
            factory->CreateSwapChainForComposition(
                device_.Get(), &description, nullptr, &swapChain_),
            "CreateSwapChainForComposition");
        configureSwapChainScale();
        check(
            compositorInterop->CreateCompositionSurfaceForSwapChain(
                swapChain_.Get(), &compositionSurface_),
            "CreateCompositionSurfaceForSwapChain");
        createRenderTarget();
    }

    void configureSwapChainScale() {
        ComPtr<IDXGISwapChain2> swapChain2;
        if (SUCCEEDED(swapChain_.As(&swapChain2))) {
            const auto inverseScale = 1.0f / scale_;
            const DXGI_MATRIX_3X2_F matrix{
                inverseScale, 0, 0, inverseScale, 0, 0};
            check(
                swapChain2->SetMatrixTransform(&matrix),
                "SetMatrixTransform");
        }
    }

    void createRenderTarget() {
        ComPtr<ID3D11Texture2D> backBuffer;
        check(
            swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)),
            "Get swap-chain back buffer");
        check(
            device_->CreateRenderTargetView(
                backBuffer.Get(), nullptr, &renderTarget_),
            "CreateRenderTargetView");
    }

    void ensureSegmentBuffer(const std::size_t count) {
        if (count <= segmentCapacity_) {
            return;
        }
        segmentCapacity_ = 256;
        while (segmentCapacity_ < count) {
            segmentCapacity_ *= 2;
        }
        segmentBuffer_.Reset();
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(
            segmentCapacity_ * sizeof(tnr::chart::Segment));
        description.Usage = D3D11_USAGE_DYNAMIC;
        description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check(
            device_->CreateBuffer(
                &description, nullptr, &segmentBuffer_),
            "Create segment buffer");
    }

    void drawGrid() {
        if ((verticalGrid_.empty() && horizontalGrid_.empty()) ||
            gridColor_.a <= 0) {
            return;
        }
        std::vector<tnr::chart::Segment> segments;
        segments.reserve(verticalGrid_.size() + horizontalGrid_.size());
        for (const auto x : verticalGrid_) {
            if (std::isfinite(x) && x >= xMinimum_ && x <= xMaximum_) {
                segments.push_back({
                    static_cast<float>(x - xMinimum_), 0,
                    static_cast<float>(x - xMinimum_), 1,
                });
            }
        }
        const auto width = static_cast<float>(xMaximum_ - xMinimum_);
        for (const auto y : horizontalGrid_) {
            if (std::isfinite(y) && y >= 0 && y <= 1) {
                segments.push_back({
                    0, static_cast<float>(y),
                    width, static_cast<float>(y),
                });
            }
        }
        drawSegments(
            segments, 0, xMaximum_ - xMinimum_, 0, 1, gridColor_, scale_);
    }

    void drawSegments(
        const std::span<const tnr::chart::Segment> segments,
        const double xMinimum,
        const double xMaximum,
        const double yMinimum,
        const double yMaximum,
        const Color color,
        const float physicalThickness) {
        if (segments.empty() || color.a <= 0) {
            return;
        }
        ensureSegmentBuffer(segments.size());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(
            context_->Map(
                segmentBuffer_.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped),
            "Map segment buffer");
        std::memcpy(
            mapped.pData,
            segments.data(),
            segments.size_bytes());
        context_->Unmap(segmentBuffer_.Get(), 0);

        DrawConstants constants{
            {static_cast<float>(xMinimum), static_cast<float>(xMaximum)},
            {static_cast<float>(yMinimum), static_cast<float>(yMaximum)},
            {static_cast<float>(width_), static_cast<float>(height_)},
            std::max(0.5f, physicalThickness),
            0,
            {color.r, color.g, color.b, color.a},
        };
        check(
            context_->Map(
                constantBuffer_.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped),
            "Map constant buffer");
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(constantBuffer_.Get(), 0);

        constexpr UINT stride = sizeof(tnr::chart::Segment);
        constexpr UINT offset = 0;
        ID3D11Buffer* vertexBuffers[] = {segmentBuffer_.Get()};
        ID3D11Buffer* constantBuffers[] = {constantBuffer_.Get()};
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetVertexBuffers(
            0, 1, vertexBuffers, &stride, &offset);
        context_->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->VSSetConstantBuffers(0, 1, constantBuffers);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, constantBuffers);
        context_->RSSetState(rasterizerState_.Get());
        constexpr float blendFactor[] = {0, 0, 0, 0};
        context_->OMSetBlendState(
            blendState_.Get(), blendFactor, 0xffffffff);
        context_->DrawInstanced(
            6, static_cast<UINT>(segments.size()), 0, 0);
    }

    SharedGraphics& graphics_{SharedGraphics::instance()};
    ComPtr<IUnknown> compositor_;
    ComPtr<IUnknown> compositionSurface_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> renderTarget_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11RasterizerState> rasterizerState_;
    ComPtr<ID3D11BlendState> blendState_;
    ComPtr<ID3D11Buffer> constantBuffer_;
    ComPtr<ID3D11Buffer> segmentBuffer_;
    std::size_t segmentCapacity_{};

    std::unordered_map<std::uint32_t, std::unique_ptr<Series>> series_;
    std::vector<std::uint32_t> seriesOrder_;
    std::uint32_t nextSeriesId_{1};
    std::vector<double> verticalGrid_;
    std::vector<double> horizontalGrid_;
    double xMinimum_{};
    double xMaximum_{1};
    Color background_{0, 0, 0, 0};
    Color gridColor_{1, 1, 1, 0.05f};
    std::uint32_t width_{1};
    std::uint32_t height_{1};
    float scale_{1};
    int deviceLossCount_{};
    std::uint64_t graphicsGeneration_{};
    std::uint64_t surfaceGeneration_{};
    std::string lastError_;
    tnr_chart_diagnostics diagnostics_{};
};

Renderer* renderer(void* chart) {
    return static_cast<Renderer*>(chart);
}

int success(const bool value) {
    return value ? 1 : 0;
}

std::size_t copyString(
    const std::string& value,
    char* buffer,
    const std::size_t bufferSize) {
    if (buffer && bufferSize > 0) {
        const auto copied = std::min(value.size(), bufferSize - 1);
        std::memcpy(buffer, value.data(), copied);
        buffer[copied] = '\0';
    }
    return value.size();
}

} // namespace

void* tnr_chart_create() {
    try {
        return new Renderer();
    } catch (...) {
        return nullptr;
    }
}

void tnr_chart_destroy(void* chart) {
    delete renderer(chart);
}

int tnr_chart_attach(
    void* chart, void* compositor, void** compositionSurface) {
    return chart
        ? success(renderer(chart)->attach(
            static_cast<IUnknown*>(compositor),
            reinterpret_cast<IUnknown**>(compositionSurface)))
        : 0;
}

int tnr_chart_resize(
    void* chart,
    const std::uint32_t width,
    const std::uint32_t height,
    const float scale) {
    return chart
        ? success(renderer(chart)->resize(width, height, scale))
        : 0;
}

void tnr_chart_set_background(
    void* chart, float r, float g, float b, float a) {
    if (chart) {
        renderer(chart)->setBackground({r, g, b, a});
    }
}

void tnr_chart_set_grid_color(
    void* chart, float r, float g, float b, float a) {
    if (chart) {
        renderer(chart)->setGridColor({r, g, b, a});
    }
}

int tnr_chart_set_x_range(void* chart, double minimum, double maximum) {
    return chart
        ? success(renderer(chart)->setXRange(minimum, maximum))
        : 0;
}

int tnr_chart_set_vertical_grid(
    void* chart, const double* values, std::size_t count) {
    return chart
        ? success(renderer(chart)->setVerticalGrid(values, count))
        : 0;
}

int tnr_chart_set_horizontal_grid(
    void* chart, const double* values, std::size_t count) {
    return chart
        ? success(renderer(chart)->setHorizontalGrid(values, count))
        : 0;
}

std::uint32_t tnr_chart_add_series(
    void* chart,
    float r,
    float g,
    float b,
    float a,
    float thickness,
    std::size_t maximumPoints,
    double maximumXSpan) {
    return chart
        ? renderer(chart)->addSeries(
            {r, g, b, a},
            thickness,
            maximumPoints,
            maximumXSpan)
        : 0;
}

int tnr_chart_remove_series(void* chart, std::uint32_t seriesId) {
    return chart
        ? success(renderer(chart)->removeSeries(seriesId))
        : 0;
}

int tnr_chart_set_series_style(
    void* chart,
    std::uint32_t seriesId,
    float r,
    float g,
    float b,
    float a,
    float thickness,
    int visible) {
    return chart
        ? success(renderer(chart)->setSeriesStyle(
            seriesId, {r, g, b, a}, thickness, visible != 0))
        : 0;
}

int tnr_chart_set_series_y_range(
    void* chart,
    std::uint32_t seriesId,
    double minimum,
    double maximum) {
    return chart
        ? success(renderer(chart)->setSeriesYRange(
            seriesId, minimum, maximum))
        : 0;
}

int tnr_chart_replace_points(
    void* chart,
    std::uint32_t seriesId,
    const tnr_chart_point* points,
    std::size_t count) {
    return chart
        ? success(renderer(chart)->replacePoints(seriesId, points, count))
        : 0;
}

int tnr_chart_append_points(
    void* chart,
    std::uint32_t seriesId,
    const tnr_chart_point* points,
    std::size_t count) {
    return chart
        ? success(renderer(chart)->appendPoints(seriesId, points, count))
        : 0;
}

int tnr_chart_clear_series(void* chart, std::uint32_t seriesId) {
    return chart
        ? success(renderer(chart)->clearSeries(seriesId))
        : 0;
}

int tnr_chart_render(void* chart) {
    return chart ? success(renderer(chart)->render()) : 0;
}

int tnr_chart_get_diagnostics(
    void* chart, tnr_chart_diagnostics* diagnostics) {
    if (!chart || !diagnostics) {
        return 0;
    }
    *diagnostics = renderer(chart)->diagnostics();
    return 1;
}

std::uint64_t tnr_chart_get_surface_generation(void* chart) {
    return chart ? renderer(chart)->surfaceGeneration() : 0;
}

std::size_t tnr_chart_copy_last_error(
    void* chart, char* buffer, std::size_t bufferSize) {
    return chart
        ? copyString(renderer(chart)->lastError(), buffer, bufferSize)
        : 0;
}

std::size_t tnr_chart_copy_adapter_name(
    void* chart, char* buffer, std::size_t bufferSize) {
    return chart
        ? copyString(renderer(chart)->adapterName(), buffer, bufferSize)
        : 0;
}
