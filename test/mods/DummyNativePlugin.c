#include <luminumbra/abi/luminumbra_abi.h>

#include <stdlib.h>
#include <string.h>

/* Function-local storage also permits the successful NULL-context fixture. */
static const LumHostApi** Host(void) {
    static const LumHostApi* host = NULL;
    return &host;
}

LUM_EXPORT bool LUM_CALL luminumbra_plugin_query(LumPluginInfo* out_info) {
    if (out_info == NULL || out_info->struct_size < sizeof(LumPluginInfo)) {
        return false;
    }
    out_info->struct_size = (uint32_t)sizeof(LumPluginInfo);
    out_info->required_abi_major = LUM_ABI_MAJOR;
    out_info->min_abi_minor = LUM_ABI_MINOR;
    out_info->rules_revision = 0;
    out_info->residency = 0;
    out_info->mod_id = "test:dummy";
    out_info->version = "1.0.0";
#if defined(LUM_TEST_BAD_MAJOR)
    out_info->required_abi_major += 1;
#elif defined(LUM_TEST_BAD_MINOR)
    out_info->min_abi_minor += 1;
#elif defined(LUM_TEST_BAD_SIZE)
    out_info->struct_size = 1;
#elif defined(LUM_TEST_QUERY_FAILURE)
    return false;
#endif
    return true;
}

LUM_EXPORT LumStatus LUM_CALL luminumbra_plugin_init(const LumHostApi* host, void** out_ctx) {
    LumProc initialized;
    if (host == NULL || out_ctx == NULL || host->struct_size < sizeof(LumHostApi) ||
        host->abi_major != LUM_ABI_MAJOR || host->get_proc_address == NULL) {
        return LUM_STATUS_ERROR;
    }
    initialized = host->get_proc_address("test_initialized");
    if (initialized == NULL || host->get_proc_address("test_unknown") != NULL) {
        return LUM_STATUS_ERROR;
    }
    *Host() = host;
    initialized();
    *out_ctx = NULL;
#if !defined(LUM_TEST_NULL_CONTEXT) && !defined(LUM_TEST_CLEAN_INIT_FAILURE)
    *out_ctx = malloc(1);
    if (*out_ctx == NULL) {
        return LUM_STATUS_ERROR;
    }
#endif
#if defined(LUM_TEST_INIT_FAILURE) || defined(LUM_TEST_CLEAN_INIT_FAILURE)
    return LUM_STATUS_ERROR;
#else
    return LUM_STATUS_OK;
#endif
}

LUM_EXPORT void LUM_CALL luminumbra_plugin_shutdown(void* ctx) {
    const LumHostApi* host = *Host();
    LumProc stopped = NULL;
    if (host != NULL) {
        stopped = host->get_proc_address("test_stopped");
    }
    if (stopped != NULL) {
        stopped();
    }
    free(ctx);
    *Host() = NULL;
}
