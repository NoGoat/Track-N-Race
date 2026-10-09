#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Opaque host. Commands are serialized by Rust; next() is a separate consumer.
void* tnr_create(void);
// Every returned allocation belongs to this module; release with tnr_free.
char* tnr_call(void* host, const char* command_json);
uint8_t* tnr_next(void* host, size_t* length);
void tnr_free(void* allocation);
void tnr_destroy(void* host);
#ifdef __cplusplus
}
#endif
