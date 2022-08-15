// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <string.h>
#include <stdbool.h>

#include "XLinkPlatform.h"
#include "XLinkPlatformErrorUtils.h"
#include "usb_host.h"
#include "pcie_host.h"
#include "tcpip_host.h"
#include "XLinkStringUtils.h"
#include "PlatformDeviceFd.h"

#define MVLOG_UNIT_NAME PlatformDeviceControl
#include "XLinkLog.h"

#ifndef USE_USB_VSC
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>

int usbFdWrite = -1;
int usbFdRead = -1;
#endif  /*USE_USB_VSC*/

#include "XLinkPublicDefines.h"

#define USB_LINK_SOCKET_PORT 5678
#define UNUSED __attribute__((unused))


static UsbSpeed_t usb_speed_enum = X_LINK_USB_SPEED_UNKNOWN;
static char mx_serial[XLINK_MAX_MX_ID_SIZE] = { 0 };
#ifdef USE_USB_VSC
static const int statuswaittimeout = 5;
#endif

#ifdef USE_TCP_IP

#if (defined(_WIN32) || defined(_WIN64))
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601  /* Windows 7. */
#endif
#include <winsock2.h>
#include <Ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#endif

#endif /* USE_TCP_IP */

// ------------------------------------
// Wrappers declaration. Begin.
// ------------------------------------

static int pciePlatformConnect(UNUSED const char *devPathRead, const char *devPathWrite, void **fd);
static int tcpipPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd);
static int tcpipPlatformServer(const char *devPathRead, const char *devPathWrite, void **fd);

static xLinkPlatformErrorCode_t usbPlatformBootBootloader(const char *name);
static int pciePlatformBootBootloader(const char *name);
static xLinkPlatformErrorCode_t tcpipPlatformBootBootloader(const char *name);

static int pciePlatformClose(void *f);
static int tcpipPlatformClose(void *fd);

static int pciePlatformBootFirmware(const deviceDesc_t* deviceDesc, const char* firmware, size_t length);
static int tcpipPlatformBootFirmware(const deviceDesc_t* deviceDesc, const char* firmware, size_t length);

// ------------------------------------
// Wrappers declaration. End.
// ------------------------------------


void xlinkSetProtocolInitialized(const XLinkProtocol_t protocol, int initialized);

// ------------------------------------
// XLinkPlatform API implementation. Begin.
// ------------------------------------

xLinkPlatformErrorCode_t XLinkPlatformInit(XLinkGlobalHandler_t* globalHandler)
{
    // Set that all protocols are initialized at first
    for(int i = 0; i < X_LINK_NMB_OF_PROTOCOLS; i++) {
        xlinkSetProtocolInitialized((const XLinkProtocol_t)i, 1);
    }

    // check for failed initialization; LIBUSB_SUCCESS = 0
    if (usbInitialize(globalHandler->options) != 0) {
        xlinkSetProtocolInitialized(X_LINK_USB_VSC, 0);
    }

    // TODO(themarpe) - move to tcpip_host
    //tcpipInitialize();
#if (defined(_WIN32) || defined(_WIN64)) && defined(USE_TCP_IP)
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2,2), &wsa_data);
#endif
    return X_LINK_PLATFORM_SUCCESS;
}


xLinkPlatformErrorCode_t XLinkPlatformBootRemote(const deviceDesc_t* deviceDesc, const char* binaryPath)
{
    FILE *file;
    long file_size;

    char *image_buffer;

    /* Open the mvcmd file */
    file = fopen(binaryPath, "rb");

    if(file == NULL) {
        mvLog(MVLOG_ERROR, "Cannot open file by path: %s", binaryPath);
        return (xLinkPlatformErrorCode_t)-7;
    }

    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    rewind(file);
    if(file_size <= 0 || !(image_buffer = (char*)malloc(file_size)))
    {
        mvLog(MVLOG_ERROR, "cannot allocate image_buffer. file_size = %ld", file_size);
        fclose(file);
        return (xLinkPlatformErrorCode_t)-3;
    }
    if((long) fread(image_buffer, 1, file_size, file) != file_size)
    {
        mvLog(MVLOG_ERROR, "cannot read file to image_buffer");
        fclose(file);
        free(image_buffer);
        return (xLinkPlatformErrorCode_t)-7;
    }
    fclose(file);

    if(XLinkPlatformBootFirmware(deviceDesc, image_buffer, file_size)) {
        free(image_buffer);
        return (xLinkPlatformErrorCode_t)-1;
    }

    free(image_buffer);
    return (xLinkPlatformErrorCode_t)0;
}

xLinkPlatformErrorCode_t XLinkPlatformBootFirmware(const deviceDesc_t* deviceDesc, const char* firmware, size_t length) {

    if(!XLinkIsProtocolInitialized(deviceDesc->protocol)) {
        return (xLinkPlatformErrorCode_t)(X_LINK_PLATFORM_DRIVER_NOT_LOADED+deviceDesc->protocol);
    }

    switch (deviceDesc->protocol) {
        case X_LINK_USB_VSC:
        case X_LINK_USB_CDC:
            return (xLinkPlatformErrorCode_t)usbPlatformBootFirmware(deviceDesc, firmware, length);

        case X_LINK_PCIE:
            return (xLinkPlatformErrorCode_t)pciePlatformBootFirmware(deviceDesc, firmware, length);

        case X_LINK_TCP_IP:
            return (xLinkPlatformErrorCode_t)tcpipPlatformBootFirmware(deviceDesc, firmware, length);

        default:
            return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }

}


xLinkPlatformErrorCode_t XLinkPlatformConnect(const char* devPathRead, const char* devPathWrite, XLinkProtocol_t protocol, void** fd)
{
    if(!XLinkIsProtocolInitialized(protocol)) {
        return (xLinkPlatformErrorCode_t)(X_LINK_PLATFORM_DRIVER_NOT_LOADED+protocol);
    }
    switch (protocol) {
        case X_LINK_USB_VSC:
        case X_LINK_USB_CDC:
            return (xLinkPlatformErrorCode_t)usbPlatformConnect(devPathRead, devPathWrite, fd);

        case X_LINK_PCIE:
            return (xLinkPlatformErrorCode_t)pciePlatformConnect(devPathRead, devPathWrite, fd);

        case X_LINK_TCP_IP:
            return (xLinkPlatformErrorCode_t)tcpipPlatformConnect(devPathRead, devPathWrite, fd);

        default:
            return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }
}

xLinkPlatformErrorCode_t XLinkPlatformServer(const char* devPathRead, const char* devPathWrite, XLinkProtocol_t protocol, void** fd)
{
    switch (protocol) {
        case X_LINK_TCP_IP:
            return (xLinkPlatformErrorCode_t)tcpipPlatformServer(devPathRead, devPathWrite, fd);

        default:
            return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }
}

xLinkPlatformErrorCode_t XLinkPlatformBootBootloader(const char* name, XLinkProtocol_t protocol)
{
    if(!XLinkIsProtocolInitialized(protocol)) {
        return (xLinkPlatformErrorCode_t)(X_LINK_PLATFORM_DRIVER_NOT_LOADED+protocol);
    }
    switch (protocol) {
        case X_LINK_USB_VSC:
        case X_LINK_USB_CDC:
            return (xLinkPlatformErrorCode_t)usbPlatformBootBootloader(name);

        case X_LINK_PCIE:
            return (xLinkPlatformErrorCode_t)pciePlatformBootBootloader(name);

        case X_LINK_TCP_IP:
            return (xLinkPlatformErrorCode_t)tcpipPlatformBootBootloader(name);

        default:
            return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }
}

xLinkPlatformErrorCode_t XLinkPlatformCloseRemote(xLinkDeviceHandle_t* deviceHandle)
{
    if(deviceHandle->protocol == X_LINK_ANY_PROTOCOL ||
       deviceHandle->protocol == X_LINK_NMB_OF_PROTOCOLS) {
        return X_LINK_PLATFORM_ERROR;
    }

    if(!XLinkIsProtocolInitialized(deviceHandle->protocol)) {
        return (xLinkPlatformErrorCode_t)(X_LINK_PLATFORM_DRIVER_NOT_LOADED+deviceHandle->protocol);
    }

    switch (deviceHandle->protocol) {
        case X_LINK_USB_VSC:
        case X_LINK_USB_CDC:
            return (xLinkPlatformErrorCode_t)usbPlatformClose(deviceHandle->xLinkFD);

        case X_LINK_PCIE:
            return (xLinkPlatformErrorCode_t)pciePlatformClose(deviceHandle->xLinkFD);

        case X_LINK_TCP_IP:
            return (xLinkPlatformErrorCode_t)tcpipPlatformClose(deviceHandle->xLinkFD);

        default:
            return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }

}

// ------------------------------------
// XLinkPlatform API implementation. End.
// ------------------------------------

/**
 * getter to obtain the connected usb speed which was stored by
 * usb_find_device_with_bcd() during XLinkconnect().
 * @note:
 *  getter will return empty or different value
 *  if called before XLinkConnect.
 */
UsbSpeed_t get_usb_speed(){
    return usb_speed_enum;
}

/**
 * getter to obtain the Mx serial id which was received by
 * usb_find_device_with_bcd() during XLinkconnect().
 * @note:
 *  getter will return empty or different value
 *  if called before XLinkConnect.
 */
const char* get_mx_serial(){
    #ifdef USE_USB_VSC
        return mx_serial;
    #else
        return "UNKNOWN";
    #endif
}

// ------------------------------------
// Helpers implementation. End.
// ------------------------------------



// ------------------------------------
// Wrappers implementation. Begin.
// ------------------------------------


int pciePlatformConnect(UNUSED const char *devPathRead,
                        const char *devPathWrite,
                        void **fd)
{
    return pcie_init(devPathWrite, fd);
}

// TODO add IPv6 to tcpipPlatformConnect()
int tcpipPlatformServer(const char *devPathRead, const char *devPathWrite, void **fd)
{
#if defined(USE_TCP_IP)

    TCPIP_SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0)
    {
        perror("socket");
        close(sock);
    }

    int reuse_addr = 1;
    int sc;
    sc = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse_addr, sizeof(int));
    if(sc < 0)
    {
        perror("setsockopt");
        close(sock);
    }

    // Disable sigpipe reception on send
    #if defined(SO_NOSIGPIPE)
        const int set = 1;
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&set, sizeof(set));
    #endif

    struct sockaddr_in serv_addr = {}, client = {};
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(TCPIP_LINK_SOCKET_PORT);
    if(bind(sock, (struct sockaddr*) &serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("bind");
        close(sock);
    }

    if(listen(sock, 1) < 0)
    {
        perror("listen");
        close(sock);
    }

    int len = sizeof(client);
    int connfd = accept(sock, (struct sockaddr*) &client, &len);
    if(connfd < 0)
    {
        perror("accept");
    }

    // Store the socket and create a "unique" key instead
    // (as file descriptors are reused and can cause a clash with lookups between scheduler and link)
    *fd = createPlatformDeviceFdKey((void*) (uintptr_t) connfd);

#else
    assert(0 && "Selected incompatible option, compile with USE_TCP_IP set");
#endif

    return 0;
}

// TODO add IPv6 to tcpipPlatformConnect()
int tcpipPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd)
{
#if defined(USE_TCP_IP)
    if (!devPathWrite || !fd) {
        return X_LINK_PLATFORM_INVALID_PARAMETERS;
    }

    TCPIP_SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);

#if (defined(_WIN32) || defined(_WIN64) )
    if(sock == INVALID_SOCKET)
    {
        return TCPIP_HOST_ERROR;
    }
#else
    if(sock < 0)
    {
        return TCPIP_HOST_ERROR;
    }
#endif

    // Disable sigpipe reception on send
    #if defined(SO_NOSIGPIPE)
        const int set = 1;
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&set, sizeof(set));
    #endif

    struct sockaddr_in serv_addr = { 0 };

    const size_t maxlen = 255;
    size_t len = strnlen(devPathWrite, maxlen + 1);
    if (len == 0 || len >= maxlen + 1)
        return X_LINK_PLATFORM_INVALID_PARAMETERS;
    char *const serv_ip = (char *)malloc(len + 1);
    if (!serv_ip)
        return X_LINK_PLATFORM_ERROR;
    serv_ip[0] = 0;
    // Parse port if specified, or use default
    int port = TCPIP_LINK_SOCKET_PORT;
    sscanf(devPathWrite, "%[^:]:%d", serv_ip, &port);

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    int ret = inet_pton(AF_INET, serv_ip, &serv_addr.sin_addr);
    free(serv_ip);

    if(ret <= 0)
    {
        tcpip_close_socket(sock);
        return (xLinkPlatformErrorCode_t)-1;
    }

    int on = 1;
    if(setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on)) < 0)
    {
        perror("setsockopt TCP_NODELAY");
        tcpip_close_socket(sock);
        return (xLinkPlatformErrorCode_t)-1;
    }

    if(connect(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
    {
        tcpip_close_socket(sock);
        return (xLinkPlatformErrorCode_t)-1;
    }

    // Store the socket and create a "unique" key instead
    // (as file descriptors are reused and can cause a clash with lookups between scheduler and link)
    *fd = createPlatformDeviceFdKey((void*) (uintptr_t) sock);

#endif
    return 0;
}


xLinkPlatformErrorCode_t usbPlatformBootBootloader(const char *name)
{
    return usbLinkBootBootloader(name);
}

int pciePlatformBootBootloader(const char *name)
{
    // TODO(themarpe)
    return (xLinkPlatformErrorCode_t)-1;
}

xLinkPlatformErrorCode_t tcpipPlatformBootBootloader(const char *name)
{
    return tcpip_boot_bootloader(name);
}


static char* pciePlatformStateToStr(const pciePlatformState_t platformState) {
    switch (platformState) {
        case PCIE_PLATFORM_ANY_STATE: return (char*)"PCIE_PLATFORM_ANY_STATE";
        case PCIE_PLATFORM_BOOTED: return (char*)"PCIE_PLATFORM_BOOTED";
        case PCIE_PLATFORM_UNBOOTED: return (char*)"PCIE_PLATFORM_UNBOOTED";
        default: return (char*)"";
    }
}
int pciePlatformClose(void *f)
{
    int rc;

    /**  For PCIe device reset is called on host side  */
#if (defined(_WIN32) || defined(_WIN64))
    rc = pcie_reset_device((HANDLE)f);
#else
    rc = pcie_reset_device(*(int*)f);
#endif
    if (rc) {
        mvLog(MVLOG_ERROR, "Device resetting failed with error %d", rc);
        pciePlatformState_t state = PCIE_PLATFORM_ANY_STATE;
        pcie_get_device_state((const char *)f, &state);
        mvLog(MVLOG_INFO, "Device state is %s", pciePlatformStateToStr(state));
    }
    rc = pcie_close(f);
    if (rc) {
        mvLog(MVLOG_ERROR, "Device closing failed with error %d", rc);
    }
    return rc;
}

int tcpipPlatformClose(void *fdKey)
{
#if defined(USE_TCP_IP)

    int status = 0;

    void* tmpsockfd = NULL;
    if(getPlatformDeviceFdFromKey(fdKey, &tmpsockfd)){
        mvLog(MVLOG_FATAL, "Cannot find file descriptor by key");
        return (xLinkPlatformErrorCode_t)-1;
    }
    TCPIP_SOCKET sock = (TCPIP_SOCKET) (uintptr_t) tmpsockfd;

#ifdef _WIN32
    status = shutdown(sock, SD_BOTH);
    if (status == 0) { status = closesocket(sock); }
#else
    if(sock != -1)
    {
        status = shutdown(sock, SHUT_RDWR);
        if (status == 0) { status = close(sock); }
    }
#endif

    if(destroyPlatformDeviceFdKey(fdKey)){
        mvLog(MVLOG_FATAL, "Cannot destroy file descriptor key");
        return (xLinkPlatformErrorCode_t)-1;
    }

    return status;

#endif
    return (xLinkPlatformErrorCode_t)-1;
}



int pciePlatformBootFirmware(const deviceDesc_t* deviceDesc, const char* firmware, size_t length){
    // Temporary open fd to boot device and then close it
    int* pcieFd = NULL;
    int rc = pcie_init(deviceDesc->name, (void**)&pcieFd);
    if (rc) {
        return rc;
    }
#if (!defined(_WIN32) && !defined(_WIN64))
    rc = pcie_boot_device(*(int*)pcieFd, firmware, length);
#else
    rc = pcie_boot_device(pcieFd, firmware, length);
#endif
    pcie_close(pcieFd); // Will not check result for now
    return rc;
}

int tcpipPlatformBootFirmware(const deviceDesc_t* deviceDesc, const char* firmware, size_t length){
    // TCPIP doesn't support a boot mechanism
    return (xLinkPlatformErrorCode_t)-1;
}

// ------------------------------------
// Wrappers implementation. End.
// ------------------------------------
