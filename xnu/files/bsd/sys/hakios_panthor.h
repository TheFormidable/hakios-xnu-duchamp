/* SPDX-License-Identifier: MIT
 * HakiOS transport v1. Wire layouts checked against Linux panthor_drm.h;
 * independently declared, no Linux implementation included. No pointers in v1.
 * Selector = DRM ioctl NR (0x40 + panthor command), NOT the whole ioctl word.
 * Inputs/outputs are inline IOConnectCallStructMethod buffers of exact size.
 */
#ifndef HAKIOS_PANTHOR_H
#define HAKIOS_PANTHOR_H
#include <stdint.h>
#define HAKIOS_PANTHOR_SERVICE "hakios_Panthor"
#define HAKIOS_PANTHOR_ABI 1u
#define HAKIOS_PANTHOR_MAX_BO (16ull << 20)
#define HAKIOS_PANTHOR_CLIENT_BYTES (64ull << 20)
#define HAKIOS_PANTHOR_GLOBAL_BYTES (128ull << 20)
#define HAKIOS_PANTHOR_HANDLES 256u
#define HAKIOS_PANTHOR_CAP_CPU_BO 1u
#define HAKIOS_PANTHOR_CAP_SUBMIT 2u
#define HAKIOS_PANTHOR_BO_NO_MMAP 1u
/* Offset token uses 16 KB alignment, independent of Linux's 4 KB GPU pages. */
#define HAKIOS_PANTHOR_TOKEN_SHIFT 14u
#define HAKIOS_PANTHOR_GEM_CLOSE 0x09u
#define HAKIOS_PANTHOR_BO_CREATE 0x45u
#define HAKIOS_PANTHOR_BO_MMAP_OFFSET 0x46u
#define HAKIOS_PANTHOR_BO_QUERY_INFO 0x50u
#define HAKIOS_PANTHOR_GET_ABI 0x80u /* HakiOS-only, not DEV_QUERY */
struct hakios_panthor_close { uint32_t handle, pad; };
struct hakios_panthor_bo_create {
    uint64_t size;
    uint32_t flags, exclusive_vm_id, handle, pad;
};
struct hakios_panthor_bo_mmap { uint32_t handle, pad; uint64_t offset; };
struct hakios_panthor_bo_info { uint32_t handle, extra_flags, create_flags, pad; };
struct hakios_panthor_abi {
    uint32_t version, capabilities, gpu_id, csf_version;
    uint32_t cpu_page_size, max_handles;
    uint64_t max_bo_size, client_byte_limit;
};
/* Immutable snapshot supplied before service publication. */
#ifdef KERNEL
#ifdef __cplusplus
extern "C" {
#endif
void hakios_panthor_start(uint32_t gpu_id, uint32_t csf_version);
#ifdef __cplusplus
}
#endif
#endif
#endif
