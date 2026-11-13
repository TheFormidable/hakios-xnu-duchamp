/* SPDX-License-Identifier: MIT
 * Duchamp user-client stage 1: CPU BO ownership and transport only.
 * No firmware/MMIO mappings, GPU VA, VM, or command submissions yet.
 * Root-only + mtkgpu_uc=1 until the isolation/scheduler stages are validated.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <mach/vm_param.h>
#include <sys/systm.h>
/* Private shared ABI: IOKit does not export arbitrary bsd/sys headers. */
#include "../../bsd/sys/hakios_panthor.h"

static_assert(sizeof(hakios_panthor_close) == 8, "close ABI");
static_assert(sizeof(hakios_panthor_bo_create) == 24, "create ABI");
static_assert(sizeof(hakios_panthor_bo_mmap) == 16, "mmap ABI");
static_assert(sizeof(hakios_panthor_bo_info) == 16, "info ABI");
static_assert(sizeof(hakios_panthor_abi) == 40, "capabilities ABI");

/* Boot-lifetime lock, allocated before publication. Lock order: client -> budget.
 * Budget follows descriptors, NOT handles: a CPU mapping can outlive GEM_CLOSE.
 */
static IOLock *hakios_budget_lock;
static uint64_t hakios_allocated;
static uint32_t hakios_gpu_id, hakios_csf_version;

class hakios_PanthorBuffer : public IOBufferMemoryDescriptor {
    OSDeclareDefaultStructors(hakios_PanthorBuffer);
    uint64_t charged;
public:
    static hakios_PanthorBuffer *make(uint64_t size);
    void free(void) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(hakios_PanthorBuffer, IOBufferMemoryDescriptor);

hakios_PanthorBuffer *hakios_PanthorBuffer::make(uint64_t size)
{
    hakios_PanthorBuffer *b = OSTypeAlloc(hakios_PanthorBuffer);
    if (!b) return NULL;
    IOLockLock(hakios_budget_lock);
    if (size > HAKIOS_PANTHOR_GLOBAL_BYTES - hakios_allocated) {
        IOLockUnlock(hakios_budget_lock);
        b->release();
        return NULL;
    }
    hakios_allocated += size;
    b->charged = size;
    IOLockUnlock(hakios_budget_lock);
    if (!b->initWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryKernelUserShared | kIOMapWriteCombineCache,
        size, PAGE_SIZE, 0) || !b->getBytesNoCopy()) {
        b->release();
        return NULL;
    }
    /* WC memory: aligned scalar stores, no DC ZVA/vectorized bzero. */
    volatile uint32_t *p = (volatile uint32_t *)b->getBytesNoCopy();
    for (uint64_t i = 0; i < size / sizeof(*p); ++i) p[i] = 0;
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    return b;
}

void hakios_PanthorBuffer::free(void)
{
    uint64_t size = charged;
    /* Hold the charge until the base class has freed backing memory. */
    IOBufferMemoryDescriptor::free();
    IOLockLock(hakios_budget_lock);
    hakios_allocated -= size;
    IOLockUnlock(hakios_budget_lock);
}

class hakios_Panthor : public IOService {
    OSDeclareDefaultStructors(hakios_Panthor);
public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(hakios_Panthor, IOService);
bool hakios_Panthor::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    if (!setProperty(kIOUserClientClassKey, "hakios_PanthorUserClient")) return false;
    setName(HAKIOS_PANTHOR_SERVICE);
    registerService();
    return true;
}

extern "C" void hakios_panthor_start(uint32_t gpu, uint32_t csf)
{
    /* Called once, by the bring-up thread, after successful termination ACK. */
    if (hakios_budget_lock) return;
    hakios_budget_lock = IOLockAlloc();
    if (!hakios_budget_lock) return;
    hakios_gpu_id = gpu;
    hakios_csf_version = csf;
    IOService *provider = IOService::getPlatform();
    hakios_Panthor *service = OSTypeAlloc(hakios_Panthor);
    if (!service || !provider || !service->init() || !service->attach(provider)) {
        OSSafeReleaseNULL(service);
        IOLog("hakios_panthor: service allocation/attachment failed\n");
        return;
    }
    if (!service->start(provider)) {
        service->detach(provider);
        IOLog("hakios_panthor: service start failed\n");
    } else {
        IOLog("hakios_panthor: ABI 1 CPU buffers ready; GPU submit unavailable\n");
    }
    service->release();
}

class hakios_PanthorUserClient : public IOUserClient {
    OSDeclareDefaultStructors(hakios_PanthorUserClient);
    struct entry { hakios_PanthorBuffer *memory; uint32_t flags; };
    entry *buffers;
    IOLock *lock;
    uint32_t nextHandle;
    uint64_t lifetimeBytes;
    bool closed;
    entry *lookup(uint32_t handle);
    void cleanup(void);
public:
    bool initWithTask(task_t task, void *security, UInt32 type,
        OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
    IOReturn clientDied(void) APPLE_KEXT_OVERRIDE;
    void free(void) APPLE_KEXT_OVERRIDE;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
        IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
        IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(hakios_PanthorUserClient, IOUserClient);

bool hakios_PanthorUserClient::initWithTask(task_t task, void *security, UInt32 type,
    OSDictionary *properties)
{
    if (!task || type != 0 || clientHasPrivilege(task, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return false;
    return IOUserClient::initWithTask(task, security, type, properties);
}
bool hakios_PanthorUserClient::start(IOService *provider)
{
    if (!OSDynamicCast(hakios_Panthor, provider) || !IOUserClient::start(provider)) return false;
    lock = IOLockAlloc();
    buffers = IONewZero(entry, HAKIOS_PANTHOR_HANDLES);
    nextHandle = 1;
    return lock && buffers;
}
hakios_PanthorUserClient::entry *hakios_PanthorUserClient::lookup(uint32_t handle)
{
    if (!buffers || handle == 0 || handle >= nextHandle || handle > HAKIOS_PANTHOR_HANDLES)
        return NULL;
    return buffers[handle - 1].memory ? &buffers[handle - 1] : NULL;
}
void hakios_PanthorUserClient::cleanup(void)
{
    if (lock) IOLockLock(lock);
    closed = true;
    if (buffers) {
        for (uint32_t i = 0; i < HAKIOS_PANTHOR_HANDLES; ++i)
            OSSafeReleaseNULL(buffers[i].memory);
    }
    if (lock) IOLockUnlock(lock);
}
IOReturn hakios_PanthorUserClient::clientClose(void)
{
    cleanup();
    terminate();
    return kIOReturnSuccess;
}
IOReturn hakios_PanthorUserClient::clientDied(void) { return clientClose(); }
void hakios_PanthorUserClient::free(void)
{
    cleanup();
    if (buffers) { IODelete(buffers, entry, HAKIOS_PANTHOR_HANDLES); buffers = NULL; }
    if (lock) { IOLockFree(lock); lock = NULL; }
    IOUserClient::free();
}
IOReturn hakios_PanthorUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
    IOMemoryDescriptor **memory)
{
    if (!options || !memory) return kIOReturnBadArgument;
    *memory = NULL;
    *options = 0;
    if (!lock) return kIOReturnNotReady;
    IOLockLock(lock);
    entry *b = closed ? NULL : lookup(type);
    IOReturn result = closed ? kIOReturnNotOpen : kIOReturnNotFound;
    if (b && !(b->flags & HAKIOS_PANTHOR_BO_NO_MMAP)) {
        b->memory->retain();
        *memory = b->memory;
        *options = kIOMapWriteCombineCache;
        result = kIOReturnSuccess;
    } else if (b) result = kIOReturnNotPermitted;
    IOLockUnlock(lock);
    return result;
}

IOReturn hakios_PanthorUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
    IOExternalMethodDispatch *, OSObject *, void *)
{
    union {
        hakios_panthor_close close;
        hakios_panthor_bo_create create;
        hakios_panthor_bo_mmap mmap;
        hakios_panthor_bo_info info;
        hakios_panthor_abi abi;
    } r = {};
    uint32_t inSize = 0, outSize = 0;
    switch (selector) {
    case HAKIOS_PANTHOR_GET_ABI: outSize = sizeof(r.abi); break;
    case HAKIOS_PANTHOR_GEM_CLOSE: inSize = sizeof(r.close); break;
    case HAKIOS_PANTHOR_BO_CREATE: inSize = outSize = sizeof(r.create); break;
    case HAKIOS_PANTHOR_BO_MMAP_OFFSET: inSize = outSize = sizeof(r.mmap); break;
    case HAKIOS_PANTHOR_BO_QUERY_INFO: inSize = outSize = sizeof(r.info); break;
    default: return kIOReturnUnsupported;
    }
    /* Validate BEFORE allocating/changing handles. No indirect or async ABI. */
    if (!a || a->scalarInputCount || a->scalarOutputCount || a->asyncWakePort ||
        a->structureInputDescriptor || a->structureOutputDescriptor || a->structureVariableOutputData ||
        a->structureInputSize != inSize || a->structureOutputSize != outSize ||
        (inSize && !a->structureInput) || (outSize && !a->structureOutput))
        return kIOReturnBadArgument;
    if (!lock || !buffers) return kIOReturnNotReady;
    if (inSize) memcpy(&r, a->structureInput, inSize);
    IOLockLock(lock);
    IOReturn result = closed ? kIOReturnNotOpen : kIOReturnSuccess;
    if (!closed) switch (selector) {
    case HAKIOS_PANTHOR_GET_ABI:
        r.abi.version = HAKIOS_PANTHOR_ABI;
        r.abi.capabilities = HAKIOS_PANTHOR_CAP_CPU_BO;
        r.abi.gpu_id = hakios_gpu_id;
        r.abi.csf_version = hakios_csf_version;
        r.abi.cpu_page_size = PAGE_SIZE;
        r.abi.max_handles = HAKIOS_PANTHOR_HANDLES;
        r.abi.max_bo_size = HAKIOS_PANTHOR_MAX_BO;
        r.abi.client_byte_limit = HAKIOS_PANTHOR_CLIENT_BYTES;
        break;
    case HAKIOS_PANTHOR_BO_CREATE: {
        if (!r.create.size || r.create.size > HAKIOS_PANTHOR_MAX_BO || r.create.pad ||
            (r.create.flags & ~HAKIOS_PANTHOR_BO_NO_MMAP)) {
            result = kIOReturnBadArgument; break;
        }
        if (r.create.exclusive_vm_id) { result = kIOReturnUnsupported; break; }
        uint64_t size = (r.create.size + PAGE_MASK) & ~(uint64_t)PAGE_MASK;
        if (size > HAKIOS_PANTHOR_CLIENT_BYTES - lifetimeBytes || nextHandle > HAKIOS_PANTHOR_HANDLES) {
            result = kIOReturnNoResources; break;
        }
        hakios_PanthorBuffer *b = hakios_PanthorBuffer::make(size);
        if (!b) { result = kIOReturnNoMemory; break; }
        /* Never recycle handles within a connection (including old mmap tokens). */
        r.create.handle = nextHandle++;
        buffers[r.create.handle - 1].memory = b;
        buffers[r.create.handle - 1].flags = r.create.flags;
        lifetimeBytes += size;
        r.create.size = size;
        break;
    }
    case HAKIOS_PANTHOR_GEM_CLOSE: {
        entry *b = lookup(r.close.handle);
        if (r.close.pad) result = kIOReturnBadArgument;
        else if (!b) result = kIOReturnNotFound;
        else OSSafeReleaseNULL(b->memory);
        break;
    }
    case HAKIOS_PANTHOR_BO_MMAP_OFFSET: {
        entry *b = lookup(r.mmap.handle);
        if (r.mmap.pad) result = kIOReturnBadArgument;
        else if (!b) result = kIOReturnNotFound;
        else if (b->flags & HAKIOS_PANTHOR_BO_NO_MMAP) result = kIOReturnNotPermitted;
        else r.mmap.offset = (uint64_t)r.mmap.handle << HAKIOS_PANTHOR_TOKEN_SHIFT;
        break;
    }
    case HAKIOS_PANTHOR_BO_QUERY_INFO: {
        entry *b = lookup(r.info.handle);
        if (r.info.pad) result = kIOReturnBadArgument;
        else if (!b) result = kIOReturnNotFound;
        else { r.info.extra_flags = 0; r.info.create_flags = b->flags; r.info.pad = 0; }
        break;
    }
    }
    if (result == kIOReturnSuccess && outSize) memcpy(a->structureOutput, &r, outSize);
    a->structureOutputSize = result == kIOReturnSuccess ? outSize : 0;
    IOLockUnlock(lock);
    return result;
}
