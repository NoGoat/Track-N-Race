#pragma once

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#  define TNR_WINUI_API extern "C" __declspec(dllexport)
#else
#  define TNR_WINUI_API extern "C"
#endif

using tnr_row_callback = void (*)(const char* json, std::size_t length, void* context);
using tnr_binary_callback = void (*)(const std::uint8_t* data, std::size_t length,
                                     void* context);
using tnr_seek_callback = void (*)(const std::uint8_t* data, std::size_t length,
                                   const char* cold_json, std::size_t cold_json_length,
                                   float current_lap_start, int lap_number, void* context);

// Protocol values: 0 = auto, 1 = F1 24, 2 = F1 25, 3 = F1 26.
TNR_WINUI_API void* tnr_engine_create(
    std::uint16_t port,
    const char* bind_address_utf8,
    int protocol,
    tnr_row_callback row_callback,
    tnr_binary_callback binary_callback,
    tnr_seek_callback seek_callback,
    void* context);

TNR_WINUI_API void tnr_engine_destroy(void* handle);
TNR_WINUI_API int tnr_engine_start(void* handle);
TNR_WINUI_API int tnr_engine_restart_udp(
    void* handle, std::uint16_t port, const char* bind_address_utf8);
TNR_WINUI_API void tnr_engine_set_protocol(void* handle, int protocol);
TNR_WINUI_API void tnr_engine_set_logging(
    void* handle, int enabled, const char* output_directory_utf8);

TNR_WINUI_API int tnr_engine_player_load(void* handle, const char* path_utf8);
TNR_WINUI_API void tnr_engine_player_play(void* handle);
TNR_WINUI_API void tnr_engine_player_pause(void* handle);
TNR_WINUI_API void tnr_engine_player_seek(void* handle, float percentage);
TNR_WINUI_API void tnr_engine_player_set_speed(void* handle, float multiplier);
TNR_WINUI_API void tnr_engine_player_get_lap_data(void* handle, int lap_number);
TNR_WINUI_API void tnr_engine_player_close(void* handle);
TNR_WINUI_API int tnr_engine_export_xlsx(
    void* handle, const char* source_path_utf8, const char* destination_path_utf8);

// Copies a NUL-terminated UTF-8 error into buffer. Returns the byte count
// (excluding NUL) required to hold the complete message.
TNR_WINUI_API std::size_t tnr_engine_copy_last_error(
    void* handle, char* buffer, std::size_t buffer_size);
TNR_WINUI_API std::size_t tnr_engine_copy_create_error(
    char* buffer, std::size_t buffer_size);
