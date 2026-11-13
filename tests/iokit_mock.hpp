/* Host-only behavioral model. NOT an XNU SDK or proof of kernel ABI correctness. */
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <atomic>
#include <cassert>
#include <cstdio>
using UInt32 = uint32_t;
using IOOptionBits = uint32_t;
using IOReturn = int;
using task_t = void *;
static task_t kernel_task = (void *)1;
static task_t test_root_task = (void *)2;
static bool test_fail_allocation = false;
static bool test_fail_lock = false;
static std::atomic<unsigned> test_live_buffers{0};
#define APPLE_KEXT_OVERRIDE override
#define OSDeclareDefaultStructors(T) public: T() = default;
#define OSDefineMetaClassAndStructors(T, B)
#define OSTypeAlloc(T) new T()
#define OSDynamicCast(T, p) dynamic_cast<T *>(p)
#define OSSafeReleaseNULL(p) do { if (p) { (p)->release(); (p) = nullptr; } } while (0)
#define IONewZero(T, n) new T[n]()
#define IODelete(p, T, n) delete[] p
#define PAGE_SIZE 16384u
#define PAGE_MASK (PAGE_SIZE - 1)
#define kIOUserClientClassKey "IOUserClientClass"
#define kIOClientPrivilegeAdministrator "root"
#define IOLog(...) ((void)0)
enum { kIOReturnSuccess, kIOReturnBadArgument, kIOReturnUnsupported, kIOReturnNotReady,
 kIOReturnNotOpen, kIOReturnNotFound, kIOReturnNotPermitted, kIOReturnNoResources, kIOReturnNoMemory };
enum { kIODirectionInOut=1, kIOMemoryKernelUserShared=2, kIOMapWriteCombineCache=4 };
using IOLock = std::mutex;
inline IOLock *IOLockAlloc() { return test_fail_lock ? nullptr : new IOLock; }
inline void IOLockFree(IOLock *p) { delete p; }
inline void IOLockLock(IOLock *p) { p->lock(); }
inline void IOLockUnlock(IOLock *p) { p->unlock(); }
class OSObject {
 std::atomic<unsigned> refs{1};
public:
 virtual ~OSObject() = default;
 virtual void free() { delete this; }
 void retain() { ++refs; }
 void release() { if (--refs == 0) free(); }
};
class OSDictionary : public OSObject {};
class IOService : public OSObject {
public:
 virtual bool init() { return true; }
 virtual bool start(IOService *) { return true; }
 bool attach(IOService *) { return true; }
 void detach(IOService *) {}
 bool setProperty(const char *, const char *) { return true; }
 void setName(const char *) {}
 void registerService() {}
 void terminate() {}
 static IOService *getPlatform() { static IOService platform; return &platform; }
};
class IOMemoryDescriptor : public OSObject {};
class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
 void *data = nullptr;
public:
 bool initWithPhysicalMask(task_t, IOOptionBits, uint64_t size, uint64_t alignment, uint64_t) {
  if (test_fail_allocation || posix_memalign(&data, alignment, size)) return false;
  memset(data, 0xab, size); ++test_live_buffers; return true;
 }
 void *getBytesNoCopy() { return data; }
 void free() override { if (data) { std::free(data); --test_live_buffers; } OSObject::free(); }
};
struct IOExternalMethodDispatch {};
struct IOExternalMethodArguments {
 const void *structureInput = nullptr;
 void *structureOutput = nullptr;
 uint32_t structureInputSize = 0, structureOutputSize = 0;
 uint32_t scalarInputCount = 0, scalarOutputCount = 0, asyncWakePort = 0;
 IOMemoryDescriptor *structureInputDescriptor = nullptr, *structureOutputDescriptor = nullptr;
 OSObject **structureVariableOutputData = nullptr;
};
class IOUserClient : public IOService {
public:
 static IOReturn clientHasPrivilege(task_t t, const char *) { return t == test_root_task ? 0 : kIOReturnNotPermitted; }
 virtual bool initWithTask(task_t, void *, UInt32, OSDictionary *) { return true; }
 virtual IOReturn clientClose() { return 0; }
 virtual IOReturn clientDied() { return clientClose(); }
 virtual IOReturn clientMemoryForType(UInt32, IOOptionBits *, IOMemoryDescriptor **) { return 0; }
 virtual IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *) { return 0; }
};
