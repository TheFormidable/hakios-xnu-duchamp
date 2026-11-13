#include "iokit_mock.hpp"
#include "../xnu/files/iokit/Kernel/hakios_Panthor.cpp"
#include <thread>
#include <vector>
static hakios_Panthor provider;
static hakios_PanthorUserClient *openClient() {
 auto *c = new hakios_PanthorUserClient();
 assert(c->initWithTask(test_root_task, nullptr, 0, nullptr));
 assert(c->start(&provider)); return c;
}
static int call(hakios_PanthorUserClient *c, uint32_t op, void *p, unsigned in, unsigned out) {
 IOExternalMethodArguments a;
 a.structureInput=p; a.structureOutput=p; a.structureInputSize=in; a.structureOutputSize=out;
 return c->externalMethod(op, &a, nullptr, nullptr, nullptr);
}
static hakios_panthor_bo_create create(hakios_PanthorUserClient *c, uint64_t size=1, uint32_t flags=0) {
 hakios_panthor_bo_create r={size, flags,0,0,0};
 assert(call(c,HAKIOS_PANTHOR_BO_CREATE,&r,sizeof(r),sizeof(r))==0); return r;
}
int main() {
 hakios_panthor_start(0x1234,0x2050000);
 auto *c=openClient(); auto *other=openClient();
 auto *denied=new hakios_PanthorUserClient();
 assert(!denied->initWithTask((void *)3,nullptr,0,nullptr));
 assert(!denied->initWithTask(test_root_task,nullptr,1,nullptr)); denied->release();
 // Failed start must be destructible.
 auto *failed=new hakios_PanthorUserClient(); test_fail_lock=true;
 assert(!failed->start(&provider)); test_fail_lock=false; failed->release();
 hakios_panthor_abi abi={};
 assert(call(c,HAKIOS_PANTHOR_GET_ABI,&abi,0,sizeof abi)==0);
 assert(abi.version==1 && abi.capabilities==HAKIOS_PANTHOR_CAP_CPU_BO && abi.gpu_id==0x1234);
 for (uint32_t op=0x40;op<=0x50;op++) if(op!=0x45&&op!=0x46&&op!=0x50)
  assert(call(c,op,nullptr,0,0)==kIOReturnUnsupported);
 // No allocation on short/oversized input, absent output, async or scalar requests.
 for (unsigned n=0;n<48;n++) if(n!=24) {
  hakios_panthor_bo_create r={1,0,0,0,0};
  assert(call(c,0x45,&r,n,24)==kIOReturnBadArgument);
 }
 hakios_panthor_bo_create r={1,0,0,0,0};
 assert(call(c,0x45,&r,24,0)==kIOReturnBadArgument);
 IOExternalMethodArguments a; a.structureInput=&r; a.structureOutput=&r;
 a.structureInputSize=24; a.structureOutputSize=24; a.asyncWakePort=1;
 assert(c->externalMethod(0x45,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
 a.asyncWakePort=0; a.scalarInputCount=1;
 assert(c->externalMethod(0x45,&a,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
 assert(hakios_allocated==0);
 for (uint64_t size : {0ull,~0ull,HAKIOS_PANTHOR_MAX_BO+1}) {
  r={size,0,0,0,0}; assert(call(c,0x45,&r,24,24)==kIOReturnBadArgument);
 }
 r={1,2,0,0,0}; assert(call(c,0x45,&r,24,24)==kIOReturnBadArgument);
 r={1,0,1,0,0}; assert(call(c,0x45,&r,24,24)==kIOReturnUnsupported);
 r={1,0,0,0,1}; assert(call(c,0x45,&r,24,24)==kIOReturnBadArgument);
 test_fail_allocation=true; r={1,0,0,0,0};
 assert(call(c,0x45,&r,24,24)==kIOReturnNoMemory);
 test_fail_allocation=false; assert(hakios_allocated==0);
 r=create(c); assert(r.handle==1 && r.size==PAGE_SIZE);
 IOMemoryDescriptor *map=nullptr; IOOptionBits options=0;
 assert(other->clientMemoryForType(r.handle,&options,&map)==kIOReturnNotFound && !map);
 assert(c->clientMemoryForType(r.handle,&options,&map)==0);
 auto *b=dynamic_cast<IOBufferMemoryDescriptor *>(map);
 for(unsigned i=0;i<PAGE_SIZE;i++) assert(((unsigned char *)b->getBytesNoCopy())[i]==0);
 ((unsigned char *)b->getBytesNoCopy())[0]=42;
 hakios_panthor_bo_mmap mm={r.handle,0,0};
 assert(call(c,0x46,&mm,16,16)==0 && mm.offset==((uint64_t)r.handle<<14));
 hakios_panthor_close close={r.handle,0}; assert(call(c,9,&close,8,0)==0);
 assert(hakios_allocated==PAGE_SIZE); // outstanding map pins descriptor and budget
 assert(((unsigned char *)b->getBytesNoCopy())[0]==42);
 assert(call(c,9,&close,8,0)==kIOReturnNotFound);
 assert(call(c,0x46,&mm,16,16)==kIOReturnNotFound);
 map->release(); assert(hakios_allocated==0);
 r=create(c,1,HAKIOS_PANTHOR_BO_NO_MMAP); assert(r.handle==2);
 assert(c->clientMemoryForType(r.handle,&options,&map)==kIOReturnNotPermitted && !map);
 hakios_panthor_bo_info info={r.handle,~0u,~0u,0};
 assert(call(c,0x50,&info,16,16)==0 && info.create_flags==1 && info.extra_flags==0);
 c->clientClose(); c->clientClose(); assert(hakios_allocated==0);
 assert(call(c,0x80,&abi,0,40)==kIOReturnNotOpen); c->release(); other->release();
 // Exhaust handles, including closes: old tokens never name new allocations.
 c=openClient();
 for(unsigned i=1;i<=256;i++) { r=create(c); assert(r.handle==i); close={i,0}; assert(call(c,9,&close,8,0)==0); }
 r={1,0,0,0,0}; assert(call(c,0x45,&r,24,24)==kIOReturnNoResources); c->release();
 // Concurrent create vs close/death, using real implementation under model locks.
 c=openClient();
 std::thread t([&]{for(int i=0;i<1000;i++) { hakios_panthor_bo_create q={1,0,0,0,0};
  int rc=call(c,0x45,&q,24,24); assert(rc==0||rc==kIOReturnNotOpen||rc==kIOReturnNoResources); }});
 c->clientDied(); t.join(); c->release(); assert(hakios_allocated==0);
 // Per-client quota, plus global quota held by maps after closing clients.
 std::vector<IOMemoryDescriptor *> maps;
 for(int client=0;client<2;client++) {
  c=openClient();
  for(int i=0;i<4;i++) { r=create(c,HAKIOS_PANTHOR_MAX_BO);
   assert(c->clientMemoryForType(r.handle,&options,&map)==0); maps.push_back(map); }
  r={1,0,0,0,0}; assert(call(c,0x45,&r,24,24)==kIOReturnNoResources); c->release();
 }
 assert(hakios_allocated==HAKIOS_PANTHOR_GLOBAL_BYTES);
 c=openClient(); r={1,0,0,0,0}; assert(call(c,0x45,&r,24,24)==kIOReturnNoMemory); c->release();
 for(auto *m:maps) m->release();
 assert(hakios_allocated==0 && test_live_buffers==0);
 puts("PASS: actual user-client C++ under host IOKit model (not an XNU kernel build)");
}
