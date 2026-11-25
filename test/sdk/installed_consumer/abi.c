#include <luminumbra/abi/luminumbra_abi.h>

int luminumbra_sdk_abi_valid(void);

int luminumbra_sdk_abi_valid(void) {
    const LumHostApi host = {sizeof(LumHostApi), LUM_ABI_MAJOR, LUM_ABI_MINOR, 0};
    return host.struct_size == sizeof(LumHostApi) && host.abi_major == 1u && host.abi_minor == 0u &&
           host.get_proc_address == 0;
}
