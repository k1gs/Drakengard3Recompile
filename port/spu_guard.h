#pragma once
#include "spu_helpers.h"
#ifdef __cplusplus
extern "C" {
#endif
void port_spu_unsupported(uint32_t pc, const char* mnemonic);
#ifdef __cplusplus
}
#endif
// The SDK otherwise logs unsupported instructions and continues with bad state.
#define spu_unsupported port_spu_unsupported
