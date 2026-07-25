#pragma once

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#  define TNR_CHART_API extern "C" __declspec(dllexport)
#else
#  define TNR_CHART_API extern "C"
#endif

struct tnr_chart_point {
    double x;
    double y;
};

struct tnr_chart_diagnostics {
    std::uint64_t source_points;
    std::uint64_t submitted_segments;
    double frame_milliseconds;
    int used_reduction;
    int using_warp;
};

TNR_CHART_API void* tnr_chart_create();
TNR_CHART_API void tnr_chart_destroy(void* chart);
TNR_CHART_API int tnr_chart_attach(
    void* chart, void* compositor, void** composition_surface);
TNR_CHART_API int tnr_chart_resize(
    void* chart, std::uint32_t width, std::uint32_t height, float scale);
TNR_CHART_API void tnr_chart_set_background(
    void* chart, float r, float g, float b, float a);
TNR_CHART_API void tnr_chart_set_grid_color(
    void* chart, float r, float g, float b, float a);
TNR_CHART_API int tnr_chart_set_x_range(
    void* chart, double minimum, double maximum);
TNR_CHART_API int tnr_chart_set_vertical_grid(
    void* chart, const double* values, std::size_t count);
TNR_CHART_API int tnr_chart_set_horizontal_grid(
    void* chart, const double* values, std::size_t count);

TNR_CHART_API std::uint32_t tnr_chart_add_series(
    void* chart,
    float r,
    float g,
    float b,
    float a,
    float thickness,
    std::size_t maximum_points,
    double maximum_x_span);
TNR_CHART_API int tnr_chart_remove_series(
    void* chart, std::uint32_t series_id);
TNR_CHART_API int tnr_chart_set_series_style(
    void* chart,
    std::uint32_t series_id,
    float r,
    float g,
    float b,
    float a,
    float thickness,
    int visible);
TNR_CHART_API int tnr_chart_set_series_y_range(
    void* chart, std::uint32_t series_id, double minimum, double maximum);
TNR_CHART_API int tnr_chart_replace_points(
    void* chart,
    std::uint32_t series_id,
    const tnr_chart_point* points,
    std::size_t count);
TNR_CHART_API int tnr_chart_append_points(
    void* chart,
    std::uint32_t series_id,
    const tnr_chart_point* points,
    std::size_t count);
TNR_CHART_API int tnr_chart_clear_series(
    void* chart, std::uint32_t series_id);

TNR_CHART_API int tnr_chart_render(void* chart);
TNR_CHART_API int tnr_chart_get_diagnostics(
    void* chart, tnr_chart_diagnostics* diagnostics);
TNR_CHART_API std::uint64_t tnr_chart_get_surface_generation(void* chart);
TNR_CHART_API std::size_t tnr_chart_copy_last_error(
    void* chart, char* buffer, std::size_t buffer_size);
TNR_CHART_API std::size_t tnr_chart_copy_adapter_name(
    void* chart, char* buffer, std::size_t buffer_size);
