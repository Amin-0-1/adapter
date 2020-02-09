/* add your code here */
#include "AppleIntelWifiAdapterV2.hpp"

#include <IOKit/IOInterruptController.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/network/IONetworkMedium.h>

#define super IOEthernetController

OSDefineMetaClassAndStructors(AppleIntelWifiAdapterV2, IOEthernetController)

enum
{
    MEDIUM_INDEX_AUTO = 0,
    MEDIUM_INDEX_10HD,
    MEDIUM_INDEX_10FD,
    MEDIUM_INDEX_100HD,
    MEDIUM_INDEX_100FD,
    MEDIUM_INDEX_100FDFC,
    MEDIUM_INDEX_1000FD,
    MEDIUM_INDEX_1000FDFC,
    MEDIUM_INDEX_1000FDEEE,
    MEDIUM_INDEX_1000FDFCEEE,
    MEDIUM_INDEX_100FDEEE,
    MEDIUM_INDEX_100FDFCEEE,
    MEDIUM_INDEX_COUNT
};

#define MBit 1000000

static IOMediumType mediumTypeArray[MEDIUM_INDEX_COUNT] = {
    kIOMediumEthernetAuto,
    (kIOMediumEthernet10BaseT | kIOMediumOptionHalfDuplex),
    (kIOMediumEthernet10BaseT | kIOMediumOptionFullDuplex),
    (kIOMediumEthernet100BaseTX | kIOMediumOptionHalfDuplex),
    (kIOMediumEthernet100BaseTX | kIOMediumOptionFullDuplex),
    (kIOMediumEthernet100BaseTX | kIOMediumOptionFullDuplex | kIOMediumOptionFlowControl),
    (kIOMediumEthernet1000BaseT | kIOMediumOptionFullDuplex),
    (kIOMediumEthernet1000BaseT | kIOMediumOptionFullDuplex | kIOMediumOptionFlowControl),
    (kIOMediumEthernet1000BaseT | kIOMediumOptionFullDuplex | kIOMediumOptionEEE),
    (kIOMediumEthernet1000BaseT | kIOMediumOptionFullDuplex | kIOMediumOptionFlowControl | kIOMediumOptionEEE),
    (kIOMediumEthernet100BaseTX | kIOMediumOptionFullDuplex | kIOMediumOptionEEE),
    (kIOMediumEthernet100BaseTX | kIOMediumOptionFullDuplex | kIOMediumOptionFlowControl | kIOMediumOptionEEE)
};

static UInt32 mediumSpeedArray[MEDIUM_INDEX_COUNT] = {
    0,
    10 * MBit,
    10 * MBit,
    100 * MBit,
    100 * MBit,
    100 * MBit,
    1000 * MBit,
    1000 * MBit,
    1000 * MBit,
    1000 * MBit,
    100 * MBit,
    100 * MBit
};

void AppleIntelWifiAdapterV2::free() {
    super::free();
    if (drv) {
        drv->release();
        delete drv;
        drv = NULL;
    }
    IOLog("Driver free()");
}

bool AppleIntelWifiAdapterV2::init(OSDictionary *properties)
{
    IOLog("Driver init()");
    drv = new IWLMvmDriver();
    return super::init(properties);
}

IOService* AppleIntelWifiAdapterV2::probe(IOService *provider, SInt32 *score)
{
    IOLog("Driver Probe()");
    super::probe(provider, score);
    IOPCIDevice *pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) {
        IOLog("Not pci device");
        return NULL;
    }
    UInt16 vendorID = pciDevice->configRead16(kIOPCIConfigVendorID);
    UInt16 deviceID = pciDevice->configRead16(kIOPCIConfigDeviceID);
    UInt16 subSystemVendorID = pciDevice->configRead16(kIOPCIConfigSubSystemVendorID);
    UInt16 subSystemDeviceID = pciDevice->configRead16(kIOPCIConfigSubSystemID);
    UInt8 revision = pciDevice->configRead8(kIOPCIConfigRevisionID);
    IOLog("find pci device====>vendorID=0x%04x, deviceID=0x%04x, subSystemVendorID=0x%04x, subSystemDeviceID=0x%04x, revision=0x%02x", vendorID, deviceID, subSystemVendorID, subSystemDeviceID, revision);
    if (!drv->init(pciDevice)) {
        return NULL;
    }
    return drv->probe() ? this : NULL;
}

bool AppleIntelWifiAdapterV2::start(IOService *provider)
{
    IOLog("Driver Start()");
    if (!super::start(provider)) {
        return false;
    }
    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop) {
        IOLog("start failed, can not create workloop\n");
        return false;
    }
    initTimeout(fWorkLoop);
    fInterrupt = IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventSource::Action, this, &AppleIntelWifiAdapterV2::intrOccured));
    fInterrupt->enable();
    IONetworkMedium *medium;
    IONetworkMedium *autoMedium;
    OSDictionary *mediumDict = OSDictionary::withCapacity(MEDIUM_INDEX_COUNT + 1);
    if (!mediumDict) {
        IOLog("start fail, can not create mediumdict\n");
        return false;
    }
    bool result;
    for (int i = MEDIUM_INDEX_AUTO; i < MEDIUM_INDEX_COUNT; i++) {
        medium = IONetworkMedium::medium(mediumTypeArray[i], mediumSpeedArray[i], 0, i);
        if (!medium) {
            IOLog("start fail, can not create mediumdict\n");
            return false;
        }
        result = IONetworkMedium::addMedium(mediumDict, medium);
        medium->release();
        if (!result) {
            IOLog("start fail, can not addMedium\n");
            return false;
        }
        if (i == MEDIUM_INDEX_AUTO) {
            autoMedium = medium;
        }
    }
    if (!publishMediumDictionary(mediumDict)) {
        IOLog("start fail, can not publish mediumdict\n");
        return false;
    }
    if (!setSelectedMedium(autoMedium)){
        IOLog("start fail, can not set current medium\n");
        return false;
    }
    if (!drv->start()) {
        IOLog("start failed\n");
        return false;
    }
    if (!attachInterface((IONetworkInterface**)&netif)) {
        IOLog("start failed, can not attach interface\n");
        return false;
    }
    netif->registerService();
    this->registerService();
    return true;
}

int AppleIntelWifiAdapterV2::intrOccured(OSObject *object, IOInterruptEventSource *, int count)
{
    drv->irqHandler(0, NULL);
    return kIOReturnOutputSuccess;
}

void AppleIntelWifiAdapterV2::stop(IOService *provider)
{
    IOLog("Driver Stop()");
    releaseTimeout();
    if (fInterrupt) {
        fInterrupt->disable();
        fWorkLoop->removeEventSource(fInterrupt);
        fInterrupt->release();
        fInterrupt = NULL;
    }
    if (fWorkLoop) {
        fWorkLoop->release();
        fWorkLoop = NULL;
    }
    if (netif) {
        detachInterface(netif);
    }
    super::stop(provider);
}

IOReturn AppleIntelWifiAdapterV2::enable(IONetworkInterface *netif)
{
    IOLog("Driver Enable()");
    return super::enable(netif);
}

IOReturn AppleIntelWifiAdapterV2::disable(IONetworkInterface *netif)
{
    IOLog("Driver Disable()");
    return super::disable(netif);
}

IOReturn AppleIntelWifiAdapterV2::setPromiscuousMode(bool active)
{
    return kIOReturnSuccess;
}

IOReturn AppleIntelWifiAdapterV2::setMulticastMode(bool active)
{
    return kIOReturnSuccess;
}

IOReturn AppleIntelWifiAdapterV2::getHardwareAddress(IOEthernetAddress *addrP) {
    addrP->bytes[0] = 0x29;
    addrP->bytes[1] = 0xC2;
    addrP->bytes[2] = 0xdd;
    addrP->bytes[3] = 0x8F;
    addrP->bytes[4] = 0x93;
    addrP->bytes[5] = 0x4D;
    return kIOReturnSuccess;
}

IOOutputQueue *AppleIntelWifiAdapterV2::createOutputQueue()
{
    if (fOutputQueue == 0) {
        fOutputQueue = IOGatedOutputQueue::withTarget(this, getWorkLoop());
    }
    return fOutputQueue;
}

UInt32 AppleIntelWifiAdapterV2::outputPacket(mbuf_t m, void *param)
{
    freePacket(m);
    return kIOReturnOutputDropped;
}
