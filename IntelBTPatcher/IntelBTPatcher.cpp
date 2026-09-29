//
//  IntelBTPatcher.cpp
//  IntelBTPatcher
//
//  Created by zxystd <zxystd@foxmail.com> on 2021/2/8.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>

#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/usb/IOUSBHostIOSource.h>
#include <IOKit/usb/IOUSBHostDevice.h>

#include "IntelBTPatcher.hpp"

static CIntelBTPatcher ibtPatcher;
static CIntelBTPatcher *callbackIBTPatcher = nullptr;

static const char *bootargOff[] {
    "-ibtcompatoff"
};

static const char *bootargDebug[] {
    "-ibtcompatdbg"
};

static const char *bootargBeta[] {
    "-ibtcompatbeta"
};

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::MountainLion,
    KernelVersion::Tahoe,
    []() {
        ibtPatcher.init();
    }
};

static const char *IntelBTPatcher_IOBluetoothFamily[] { "/System/Library/Extensions/IOBluetoothFamily.kext/Contents/MacOS/IOBluetoothFamily" };

static KernelPatcher::KextInfo IntelBTPatcher_IOBluetoothInfo {
    "com.apple.iokit.IOBluetoothFamily",
    IntelBTPatcher_IOBluetoothFamily,
    1,
    {true, true},
    {},
    KernelPatcher::KextInfo::Unloaded
};

static const char *IntelBTPatcher_IOUSBHostFamily[] {
    "/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily" };

static KernelPatcher::KextInfo IntelBTPatcher_IOUsbHostInfo {
    "com.apple.iokit.IOUSBHostFamily",
    IntelBTPatcher_IOUSBHostFamily,
    1,
    {true, true},
    {},
    KernelPatcher::KextInfo::Unloaded
};

bool CIntelBTPatcher::_randomAddressInit = false;

bool CIntelBTPatcher::init()
{
    DBGLOG(DRV_NAME, "%s", __PRETTY_FUNCTION__);
    callbackIBTPatcher = this;
    if (getKernelVersion() < KernelVersion::Monterey) {
        lilu.onKextLoadForce(&IntelBTPatcher_IOBluetoothInfo, 1,
        [](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            callbackIBTPatcher->processKext(patcher, index, address, size);
        }, this);
    } else {
        lilu.onKextLoadForce(&IntelBTPatcher_IOUsbHostInfo, 1,
        [](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            callbackIBTPatcher->processKext(patcher, index, address, size);
        }, this);
    }
    return true;
}

void CIntelBTPatcher::free()
{
    DBGLOG(DRV_NAME, "%s", __PRETTY_FUNCTION__);
}

void CIntelBTPatcher::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)
{
    DBGLOG(DRV_NAME, "%s", __PRETTY_FUNCTION__);
    if (getKernelVersion() < KernelVersion::Monterey) {
        if (IntelBTPatcher_IOBluetoothInfo.loadIndex == index) {
            DBGLOG(DRV_NAME, "%s", IntelBTPatcher_IOBluetoothInfo.id);
            
            KernelPatcher::RouteRequest findQueueRequestRequest {
                "__ZN25IOBluetoothHostController17FindQueuedRequestEtP22BluetoothDeviceAddresstbPP21IOBluetoothHCIRequest",
                newFindQueueRequest,
                oldFindQueueRequest
            };
            patcher.routeMultiple(index, &findQueueRequestRequest, 1, address, size);
            if (patcher.getError() == KernelPatcher::Error::NoError) {
                DBGLOG(DRV_NAME, "routed %s", findQueueRequestRequest.symbol);
            } else {
                SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", findQueueRequestRequest.symbol, patcher.getError());
                patcher.clearError();
            }
            
        }
    } else {
        if (IntelBTPatcher_IOUsbHostInfo.loadIndex == index) {
            SYSLOG(DRV_NAME, "%s", IntelBTPatcher_IOUsbHostInfo.id);
            
            KernelPatcher::RouteRequest hostDeviceRequest {
            "__ZN15IOUSBHostDevice13deviceRequestEP9IOServiceRN11StandardUSB13DeviceRequestEPvP18IOMemoryDescriptorRjP19IOUSBHostCompletionj",
                newHostDeviceRequest,
                oldHostDeviceRequest
            };
            patcher.routeMultiple(index, &hostDeviceRequest, 1, address, size);
            if (patcher.getError() == KernelPatcher::Error::NoError) {
                SYSLOG(DRV_NAME, "routed %s", hostDeviceRequest.symbol);
            } else {
                SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", hostDeviceRequest.symbol, patcher.getError());
                patcher.clearError();
            }

            /* Intercept bulk/interrupt pipe async completions so the
             * unsupported Broadcom 0xfc79 response can be rewritten. */
            KernelPatcher::RouteRequest pipeIoRequest {
                "__ZN14IOUSBHostPipe2ioEP18IOMemoryDescriptorjP19IOUSBHostCompletionj",
                newPipeIo,
                oldPipeIo
            };
            patcher.routeMultiple(index, &pipeIoRequest, 1, address, size);
            if (patcher.getError() == KernelPatcher::Error::NoError) {
                SYSLOG(DRV_NAME, "routed %s", pipeIoRequest.symbol);
            } else {
                SYSLOG(DRV_NAME, "failed to resolve %s, error = %d", pipeIoRequest.symbol, patcher.getError());
                patcher.clearError();
            }
        }
    }
}

#pragma mark - For Bigsur-, patch unhandled 0x2019 opcode

#define HCI_OP_LE_START_ENCRYPTION 0x2019

IOReturn CIntelBTPatcher::newFindQueueRequest(void *that, unsigned short arg1, void *addr, unsigned short arg2, bool arg3, void **hciRequestPtr)
{
    IOReturn ret = FunctionCast(newFindQueueRequest, callbackIBTPatcher->oldFindQueueRequest)(that, arg1, addr, arg2, arg3, hciRequestPtr);
    if (ret != 0 && arg1 == HCI_OP_LE_START_ENCRYPTION) {
        ret = FunctionCast(newFindQueueRequest, callbackIBTPatcher->oldFindQueueRequest)(that, arg1, addr, 0xffff, arg3, hciRequestPtr);
        DBGLOG(DRV_NAME, "%s ret: %d arg1: 0x%04x arg2: 0x%04x arg3: %d ptr: %p", __FUNCTION__, ret, arg1, arg2, arg3, *hciRequestPtr);
    }
    return ret;
}

#pragma mark - For Monterey+ patch for intercept HCI REQ and RESP

StandardUSB::DeviceRequest randomAddressRequest;
// Hardcoded Random Address HCI Command
const uint8_t randomAddressHci[9] = {0x05, 0x20, 0x06, 0x94, 0x50, 0x64, 0xD0, 0x78, 0x6B}; 
IOBufferMemoryDescriptor *writeHCIDescriptor = nullptr;

#define MAX_HCI_BUF_LEN             255
#define HCI_OP_RESET                0x0c03
#define HCI_OP_LE_SET_SCAN_PARAM    0x200B
#define HCI_OP_LE_SET_SCAN_ENABLE   0x200C

IOReturn CIntelBTPatcher::newHostDeviceRequest(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data, IOMemoryDescriptor *descriptor, unsigned int &length, IOUSBHostCompletion *completion, unsigned int timeout)
{
    HciCommandHdr *hdr = nullptr;
    uint32_t hdrLen = 0;
    char hciBuf[MAX_HCI_BUF_LEN] = {0};
    
    if (data == nullptr) {
        if (descriptor != nullptr &&
            (getKernelVersion() < KernelVersion::Sequoia || !descriptor->prepare(kIODirectionOut))) {
            if (descriptor->getLength() > 0) {
                descriptor->readBytes(0, hciBuf, min(descriptor->getLength(), MAX_HCI_BUF_LEN));
                hdrLen = (uint32_t)min(descriptor->getLength(), MAX_HCI_BUF_LEN);
            }
            if (getKernelVersion() >= KernelVersion::Sequoia)
                descriptor->complete(kIODirectionOut);
        }
        hdr = (HciCommandHdr *)hciBuf;
        if (hdr->opcode == HCI_OP_LE_SET_SCAN_PARAM) {
            if (!_randomAddressInit) {
                randomAddressRequest.bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionOut, kRequestTypeClass, kRequestRecipientInterface);
                randomAddressRequest.bRequest = 0xE0;
                randomAddressRequest.wIndex = 0;
                randomAddressRequest.wValue = 0;
                randomAddressRequest.wLength = 9;
                length = 9;
                if (writeHCIDescriptor == nullptr)
                    writeHCIDescriptor = IOBufferMemoryDescriptor::withBytes(randomAddressHci, 9, kIODirectionOut);
                writeHCIDescriptor->prepare(kIODirectionOut);
                IOReturn ret = FunctionCast(newHostDeviceRequest, callbackIBTPatcher->oldHostDeviceRequest)(that, provider, randomAddressRequest, nullptr, writeHCIDescriptor, length, nullptr, timeout);
                writeHCIDescriptor->complete();
                const char *randAddressDump = _hexDumpHCIData((uint8_t *)randomAddressHci, 9);
                if (randAddressDump) {
                    SYSLOG(DRV_NAME, "[PATCH] Sending Random Address HCI %lld %s", ret, randAddressDump);
                    IOFree((void *)randAddressDump, 9 * 3 + 1);
                }
                _randomAddressInit = true;
                SYSLOG(DRV_NAME, "[PATCH] Resend LE SCAN PARAM HCI %lld", ret);
            }
        }
    } else {
        hdr = (HciCommandHdr *)data;
        hdrLen = request.wLength - 3;
    }
    if (hdr) {
        // HCI reset, we need to send Random address again
        if (hdr->opcode == HCI_OP_RESET)
            _randomAddressInit = false;
#if DEBUG
        DBGLOG(DRV_NAME, "[%s] bRequest: 0x%x direction: %s type: %s recipient: %s wValue: 0x%02x wIndex: 0x%02x opcode: 0x%04x len: %d length: %d async: %d", provider->getName(), request.bRequest, requestDirectionNames[(request.bmRequestType & kDeviceRequestDirectionMask) >> kDeviceRequestDirectionPhase], requestRecipientNames[(request.bmRequestType & kDeviceRequestRecipientMask) >> kDeviceRequestRecipientPhase], requestTypeNames[(request.bmRequestType & kDeviceRequestTypeMask) >> kDeviceRequestTypePhase], request.wValue, request.wIndex, hdr->opcode, hdr->len, request.wLength, completion != nullptr);
        if (hdrLen) {
            const char *dump = _hexDumpHCIData((uint8_t *)hdr, hdrLen);
            if (dump) {
                DBGLOG(DRV_NAME, "[Request]: %s", dump);
                IOFree((void *)dump, hdrLen * 3 + 1);
            }
        }
#endif
    }
    return FunctionCast(newHostDeviceRequest, callbackIBTPatcher->oldHostDeviceRequest)(that, provider, request, data, descriptor, length, completion, timeout);
}

#pragma mark - Sequoia: rewrite Broadcom 0xfc79 error on interrupt-IN

/* The Intel AX200 replies to the Broadcom-specific "Read Verbose Config
 * Version Info" VSC (0xfc79), which Sequoia's bluetoothd issues during
 * Stack_Init, with a Command Complete carrying an error status and no
 * payload. bluetoothd treats that as a fatal transport failure
 * (reason=108) and keeps restarting. We wrap the interrupt-IN pipe async
 * completion for the AX200 and, when that exact error event arrives,
 * replace it in place with a successful Broadcom response so init proceeds.
 */

struct PipeIoCtx {
    IOUSBHostCompletion origCompletion;
    IOMemoryDescriptor *buffer;
};

IOReturn CIntelBTPatcher::
newPipeIo(void *that, void *dataBuffer, uint32_t dataBufferLength,
          void *completion, uint32_t completionTimeoutMs)
{
    using PipeIoFn = IOReturn (*)(void *, void *, uint32_t, void *, uint32_t);
    PipeIoFn real = FunctionCast(newPipeIo, callbackIBTPatcher->oldPipeIo);

    IOUSBHostCompletion *comp = (IOUSBHostCompletion *)completion;
    IOMemoryDescriptor *buf = (IOMemoryDescriptor *)dataBuffer;

    /* Fast path: only wrap asynchronous reads with room for the 12-byte
     * replacement event; everything else is passed through untouched. */
    if (!comp || !comp->action || !buf || dataBufferLength < 12)
        return real(that, dataBuffer, dataBufferLength, completion, completionTimeoutMs);

    /* Restrict wrapping to the AX200 so every other USB device is unaffected
     * and allocates nothing. */
    IOUSBHostPipe *pipe = (IOUSBHostPipe *)that;
    bool ax200 = false;
    if (pipe) {
        IOUSBHostDevice *dev = pipe->getDevice();
        if (dev) {
            const StandardUSB::DeviceDescriptor *dd = dev->getDeviceDescriptor();
            if (dd && USBToHost16(dd->idVendor) == 0x8087 &&
                USBToHost16(dd->idProduct) == 0x0029)
                ax200 = true;
        }
    }
    if (!ax200)
        return real(that, dataBuffer, dataBufferLength, completion, completionTimeoutMs);

    PipeIoCtx *ctx = (PipeIoCtx *)IOMalloc(sizeof(PipeIoCtx));
    if (!ctx)
        return real(that, dataBuffer, dataBufferLength, completion, completionTimeoutMs);

    ctx->origCompletion = *comp;
    ctx->buffer = buf;

    IOUSBHostCompletion wrapped;
    wrapped.owner = ctx;
    wrapped.action = pipeIoAction;
    wrapped.parameter = nullptr;

    IOReturn ret = real(that, dataBuffer, dataBufferLength, &wrapped, completionTimeoutMs);
    if (ret != kIOReturnSuccess) {
        /* Request not accepted; the completion will never run. */
        IOFree(ctx, sizeof(PipeIoCtx));
        return ret;
    }
    return ret;
}

void CIntelBTPatcher::
pipeIoAction(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred)
{
    PipeIoCtx *ctx = (PipeIoCtx *)owner;
    uint32_t reported = bytesTransferred;

    if (ctx && status == kIOReturnSuccess && ctx->buffer &&
        bytesTransferred >= 6 && ctx->buffer->getLength() >= 12) {
        uint8_t hdr[6] = {};
        if (ctx->buffer->readBytes(0, hdr, sizeof(hdr)) == (IOByteCount)sizeof(hdr) &&
            hdr[0] == 0x0e &&                    /* HCI Command Complete */
            hdr[3] == 0x79 && hdr[4] == 0xfc &&  /* opcode 0xfc79 (LE) */
            hdr[5] != 0x00) {                    /* chip returned an error */
            /* Successful Broadcom Read Verbose Config Version Info:
             * status=0 followed by 6 bytes (chip id / revision / build). */
            static const uint8_t kFakeVerboseConfigCC[12] = {
                0x0e, 0x0a, 0x01, 0x79, 0xfc, 0x00,
                0xe8, 0x03, 0x00, 0x00, 0x39, 0x15
            };
            if (ctx->buffer->writeBytes(0, kFakeVerboseConfigCC,
                                        sizeof(kFakeVerboseConfigCC))
                == (IOByteCount)sizeof(kFakeVerboseConfigCC))
                reported = (uint32_t)sizeof(kFakeVerboseConfigCC);
        }
    }

    if (ctx) {
        if (ctx->origCompletion.action)
            ctx->origCompletion.action(ctx->origCompletion.owner, parameter,
                                       status, reported);
        IOFree(ctx, sizeof(PipeIoCtx));
    }
}
