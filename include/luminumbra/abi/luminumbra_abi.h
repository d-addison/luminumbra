#ifndef LUMINUMBRA_ABI_H
#define LUMINUMBRA_ABI_H

#include <stdbool.h>
#include <stdint.h>

#define LUM_ABI_MAJOR 1u
#define LUM_ABI_MINOR 0u

#if defined(_WIN32)
#if defined(LUM_PLUGIN_BUILD)
#define LUM_EXPORT __declspec(dllexport)
#else
#define LUM_EXPORT
#endif
#define LUM_CALL __cdecl
#else
#define LUM_EXPORT __attribute__((visibility("default")))
#define LUM_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LumSlice {
    const uint8_t* data;
    uint64_t len;
} LumSlice;

typedef struct LumStrView {
    const char* data;
    uint64_t len;
} LumStrView;

typedef uint64_t LumEntityId;
typedef uint32_t LumComponentId;
typedef int32_t LumStatus;

#define LUM_STATUS_OK 0
#define LUM_STATUS_ERROR (-1)

typedef void(LUM_CALL* LumProc)(void);
typedef LumProc(LUM_CALL* LumGetProcAddressFn)(const char* proc_name);

typedef struct LumHostApi {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    LumGetProcAddressFn get_proc_address;
} LumHostApi;

typedef struct LumPluginInfo {
    uint32_t struct_size;
    uint32_t required_abi_major;
    uint32_t min_abi_minor;
    uint32_t rules_revision;
    uint8_t residency; /* 0: client; 1: authoritative. */
    const char* mod_id;
    const char* version;
} LumPluginInfo;

typedef bool(LUM_CALL* LumPluginQueryFn)(LumPluginInfo* out_info);
typedef LumStatus(LUM_CALL* LumPluginInitFn)(const LumHostApi* host, void** out_ctx);
typedef void(LUM_CALL* LumPluginShutdownFn)(void* ctx);

/* Define LUM_PLUGIN_BUILD when compiling a plugin to export its entry points.
 * Query fills only the caller's struct_size bytes. Strings remain plugin-owned.
 * A compatible major and a supported minimum minor are required before init.
 * The host table remains valid until shutdown. All callbacks must return normally:
 * exceptions, C++ objects and allocator ownership must never cross this boundary.
 * The plugin owns its context. On init failure it rolls back and leaves it NULL;
 * the host also calls shutdown if a failed init returns a non-NULL context.
 * Shutdown is called exactly once after successful init, including a NULL context.
 * Session owners destroy the loader only after all plugin callbacks have stopped.
 */
LUM_EXPORT bool LUM_CALL luminumbra_plugin_query(LumPluginInfo* out_info);
LUM_EXPORT LumStatus LUM_CALL luminumbra_plugin_init(const LumHostApi* host, void** out_ctx);
LUM_EXPORT void LUM_CALL luminumbra_plugin_shutdown(void* ctx);

#ifdef __cplusplus
}
#endif

#endif
