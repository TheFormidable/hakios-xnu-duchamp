/* SPDX-License-Identifier: MIT */
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>

extern "C" bool hakios_mtk_bt_publish_address(const uint8_t *address);

bool
hakios_mtk_bt_publish_address(const uint8_t *address)
{
	IOService *platform = IOService::getPlatform();
	IORegistryEntry *parent = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IOService *node = OSTypeAlloc(IOService);
	OSData *mac = OSData::withBytes(address, 6);
	bool published = false;

	if (platform && parent && node && mac && node->init() && node->attach(platform)) {
		node->setName("bluetooth");
		node->setName("bluetooth", gIODTPlane);
		if (node->setProperty("local-mac-address", mac) &&
		    node->attachToParent(parent, gIODTPlane)) {
            /* Address consumers can use the DT node now. Publish to IOKit
             * matching only after H4 registration and the endpoint succeed. */
			published = true;
		} else {
			node->detach(platform);
		}
	}
	OSSafeReleaseNULL(node);
	OSSafeReleaseNULL(parent);
	OSSafeReleaseNULL(mac);
	return published;
}

extern "C" bool hakios_mtk_bt_publish_ready(void);

bool
hakios_mtk_bt_publish_ready(void)
{
    IORegistryEntry *entry = IORegistryEntry::fromPath("/arm-io/bluetooth", gIODTPlane);
    IOService *node = OSDynamicCast(IOService, entry);
    bool ready = node && node->setProperty("hakios-native-hci-ready", true);
    if (ready)
        node->registerService();
    OSSafeReleaseNULL(entry);
    return ready;
}

/*
 * The POCO X6 Pro has a vibration motor (an AW86927, drivers/haptic/hakios.haptic.c): what xnuloader's tree says for the
 * redwood phone under /product, "haptics" with closed-loop and supports-3rd-party-haptics, is said here too (the duchamp
 * loader's tree has no such node). With it MobileGestalt answers DeviceSupportsClosedLoopHaptics, UIKit's feedbacks and
 * CoreHaptics play their waveforms on the audio plug-in's "Actuator" device, and the motor driver gets them.
 */
extern "C" bool hakios_mtk_publish_haptics(void);

bool
hakios_mtk_publish_haptics(void)
{
	IORegistryEntry *product = IORegistryEntry::fromPath("/product", gIODTPlane);
	IORegistryEntry *old = product ? IORegistryEntry::fromPath("/product/haptics", gIODTPlane) : NULL;
	IORegistryEntry *node = NULL;
	uint32_t one = 1;
	OSData *yes = OSData::withBytes(&one, sizeof(one));
	bool published = false;

	if (old) {
		published = true;               /* there already */
	} else if (product && yes && (node = OSTypeAlloc(IORegistryEntry)) && node->init()) {
		node->setName("haptics");
		node->setName("haptics", gIODTPlane);
		if (node->setProperty("closed-loop", yes) && node->setProperty("supports-3rd-party-haptics", yes) &&
		    node->attachToParent(product, gIODTPlane)) {
			published = true;
		}
	}
	OSSafeReleaseNULL(node);
	OSSafeReleaseNULL(old);
	OSSafeReleaseNULL(product);
	OSSafeReleaseNULL(yes);
	return published;
}
