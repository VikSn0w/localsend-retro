/* The parts of Open Transport and the Thread Manager this app uses, declared by hand.
 * The open-source headers that come with the Retro68 cross-compiler have neither, while CarbonLib exports both.
 * Layouts and values are those of Apple's OpenTransport.h, OpenTransportProviders.h and Threads.h. Plain C. */
#ifndef LS_OT_DECL_H
#define LS_OT_DECL_H

#include <MacTypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef SInt32 ls_OSStatus;
typedef struct OpaqueOTClientContextPtr *OTClientContextPtr;
typedef void *OTConfigurationRef;
typedef void *EndpointRef;
typedef UInt32 OTByteCount;
typedef SInt32 OTResult;
typedef UInt32 OTFlags;
typedef SInt32 OTSequence;
typedef UInt32 OTOpenFlags;
typedef UInt32 OTXTILevel;
typedef UInt32 OTXTIName;

typedef struct TNetbuf {
    OTByteCount maxlen;
    OTByteCount len;
    UInt8 *buf;
} TNetbuf;

typedef struct TCall {
    TNetbuf addr;
    TNetbuf opt;
    TNetbuf udata;
    OTSequence sequence;
} TCall;

typedef struct TBind {
    TNetbuf addr;
    UInt32 qlen;
} TBind;

typedef struct TDiscon {
    TNetbuf udata;
    SInt32 reason;
    OTSequence sequence;
} TDiscon;

typedef struct TUnitData {
    TNetbuf addr;
    TNetbuf opt;
    TNetbuf udata;
} TUnitData;

typedef struct TUDErr {
    TNetbuf addr;
    TNetbuf opt;
    SInt32 error;
} TUDErr;

typedef struct TOptMgmt {
    TNetbuf opt;
    OTFlags flags;
} TOptMgmt;

typedef struct TOptionHeader {
    OTByteCount len;      /* header plus value, in bytes */
    OTXTILevel level;
    OTXTIName name;
    UInt32 status;
} TOptionHeader;

typedef struct TIPAddMulticast {
    UInt32 multicastGroupAddress;
    UInt32 interfaceAddress;
} TIPAddMulticast;

typedef struct InetAddress {
    UInt16 fAddressType;  /* AF_INET = 2 */
    UInt16 fPort;
    UInt32 fHost;
    UInt8 fUnused[8];
} InetAddress;

typedef struct InetHostInfo {
    char name[256];
    UInt32 addrs[10];
} InetHostInfo;

enum {
    kInitOTForApplicationMask = 1,
    kOTNoError = 0,
    kOTNoDataErr = -3162,
    kOTLookErr = -3158,
    kOTFlowErr = -3161,
    kOTOutStateErr = -3155,
    kOTNoDisconnectErr = -3163,
    kOTNoReleaseErr = -3164,
    kOTBadSyncErr = -3160,

    T_LISTEN = 0x0001,
    T_CONNECT = 0x0002,
    T_DATA = 0x0004,
    T_DISCONNECT = 0x0010,
    T_UDERR = 0x0040,
    T_ORDREL = 0x0080,
    T_GODATA = 0x0100,
    T_PASSCON = 0x1000,

    T_NEGOTIATE = 0x0004,
    kOTAF_INET = 2,
    INET_IP = 0x00,
    kIP_REUSEADDR = 0x04,
    kIP_MULTICAST_TTL = 0x1011,
    kIP_ADD_MEMBERSHIP = 0x1013
};

ls_OSStatus InitOpenTransportInContext(UInt32 flags, OTClientContextPtr *outContext);
void CloseOpenTransportInContext(OTClientContextPtr context);
OTConfigurationRef OTCreateConfiguration(const char *path);
EndpointRef OTOpenEndpointInContext(OTConfigurationRef config, OTOpenFlags flags, void *info, ls_OSStatus *err,
                                    OTClientContextPtr context);
ls_OSStatus OTSetSynchronous(void *provider);
ls_OSStatus OTSetNonBlocking(void *provider);
ls_OSStatus OTCloseProvider(void *provider);
OTResult OTBind(EndpointRef ep, TBind *request, TBind *returned);
OTResult OTConnect(EndpointRef ep, TCall *send, TCall *receive);
OTResult OTRcvConnect(EndpointRef ep, TCall *call);
OTResult OTListen(EndpointRef ep, TCall *call);
OTResult OTAccept(EndpointRef listener, EndpointRef worker, TCall *call);
OTResult OTSnd(EndpointRef ep, void *buffer, OTByteCount bytes, OTFlags flags);
OTResult OTRcv(EndpointRef ep, void *buffer, OTByteCount bytes, OTFlags *flags);
OTResult OTLook(EndpointRef ep);
OTResult OTRcvDisconnect(EndpointRef ep, TDiscon *discon);
OTResult OTRcvOrderlyDisconnect(EndpointRef ep);
OTResult OTSndOrderlyDisconnect(EndpointRef ep);
OTResult OTSndDisconnect(EndpointRef ep, TCall *call);
OTResult OTSndUData(EndpointRef ep, TUnitData *udata);
OTResult OTRcvUData(EndpointRef ep, TUnitData *udata, OTFlags *flags);
OTResult OTRcvUDErr(EndpointRef ep, TUDErr *uderr);
OTResult OTOptionManagement(EndpointRef ep, TOptMgmt *req, TOptMgmt *ret);
/* Fills (at least) five hosts: this Mac's address, netmask, broadcast, default router and DNS server.
 * Pass a zeroed buffer of 1 KB and kDefaultInetInterface (-1). */
ls_OSStatus OTInetGetInterfaceInfo(void *info, SInt32 index);

/* ---- Thread Manager (cooperative threads) ---- */
typedef UInt32 ThreadStyle;
typedef UInt32 ThreadID;
typedef UInt32 ThreadOptions;
typedef void *(*ThreadEntryProcPtr)(void *threadParam);
enum { kCooperativeThread = 1, kCreateIfNeeded = 4 };
OSErr NewThread(ThreadStyle style, ThreadEntryProcPtr entry, void *param, long stackSize, ThreadOptions options,
                void **result, ThreadID *made);
OSErr YieldToAnyThread(void);

#ifdef __cplusplus
}
#endif

#endif
