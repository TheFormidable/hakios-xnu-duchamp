/* SPDX-License-Identifier: MIT
 * Run as root ON duchamp's HakiOS. CPU mapping test, no GPU commands or MMIO.
 * Compile with the project's Darwin/iOS toolchain and IOKit user headers.
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/sys/hakios_panthor.h"
int main(void)
{
    io_connect_t connection = IO_OBJECT_NULL;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
        IOServiceMatching(HAKIOS_PANTHOR_SERVICE));
    mach_vm_address_t address = 0;
    mach_vm_size_t length = 0;
    struct hakios_panthor_bo_create bo = {0};
    int result = 1;
    kern_return_t kr;
    if (!service) { fprintf(stderr, "No hakios_Panthor service: check mtkgpu=1 mtkgpu_uc=1 and termination ACK.\n"); return 1; }
    kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (kr) { fprintf(stderr,"IOServiceOpen: %#x (root required)\n",kr); return 1; }
    struct hakios_panthor_abi abi = {0};
    size_t size = sizeof(abi);
    kr = IOConnectCallStructMethod(connection, HAKIOS_PANTHOR_GET_ABI, NULL, 0, &abi, &size);
    if (kr || size != sizeof(abi) || abi.version != 1 || !(abi.capabilities & HAKIOS_PANTHOR_CAP_CPU_BO)) goto done;
    printf("ABI %u GPU %#x CSF %#x CPU page %u; submit %s\n", abi.version, abi.gpu_id, abi.csf_version,
        abi.cpu_page_size, (abi.capabilities & HAKIOS_PANTHOR_CAP_SUBMIT) ? "yes" : "unavailable");
    bo.size = 1;
    size = sizeof(bo);
    kr = IOConnectCallStructMethod(connection, HAKIOS_PANTHOR_BO_CREATE, &bo, sizeof(bo), &bo, &size);
    if (kr || size != sizeof(bo) || bo.size != abi.cpu_page_size || !bo.handle) goto done;
    struct hakios_panthor_bo_mmap token = {bo.handle, 0, 0};
    size = sizeof(token);
    kr = IOConnectCallStructMethod(connection, HAKIOS_PANTHOR_BO_MMAP_OFFSET, &token, sizeof(token), &token, &size);
    if (kr || token.offset != ((uint64_t)bo.handle << HAKIOS_PANTHOR_TOKEN_SHIFT)) goto done;
    kr = IOConnectMapMemory64(connection, bo.handle, mach_task_self(), &address, &length, kIOMapAnywhere);
    if (kr || length < bo.size) goto done;
    volatile uint32_t *words = (volatile uint32_t *)(uintptr_t)address;
    for (uint64_t n = 0; n < bo.size / 4; n++) if (words[n]) goto done;
    words[0] = 0xc0ffee01;
    words[bo.size / 4 - 1] = 0x600dcafe;
    if (words[0] != 0xc0ffee01 || words[bo.size / 4 - 1] != 0x600dcafe) goto done;
    result = 0;
    puts("PASS: allocated, zeroed, mapped and CPU-written BO (not a GPU execution test)");
done:
    if (address) IOConnectUnmapMemory64(connection, bo.handle, mach_task_self(), address);
    if (bo.handle) {
        struct hakios_panthor_close close = {bo.handle, 0};
        kern_return_t close_kr = IOConnectCallStructMethod(connection, HAKIOS_PANTHOR_GEM_CLOSE,
            &close, sizeof(close), NULL, NULL);
        if (close_kr) { fprintf(stderr, "GEM_CLOSE: %#x\n", close_kr); result = 1; }
    }
    IOServiceClose(connection);
    if (result) fprintf(stderr,"FAIL: transport/mapping check, last IOReturn %#x\n",kr);
    return result;
}
