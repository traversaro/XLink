#include <cstdio>
#include <string>
#include <vector>
#include <array>
#include <thread>
#include <stdexcept>
#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstring>
#include <atomic>
#include <condition_variable>

#include "XLink/XLink.h"
#include "XLink/XLinkPublicDefines.h"
#include "XLink/XLinkLog.h"


#ifdef XLINK_TEST_CLIENT
// Client
int main(int argc, char** argv) {

    XLinkGlobalHandler_t gHandler;
    XLinkInitialize(&gHandler);

    int numConnections = 1;
    std::string localhost = "127.0.0.1";
    char* tmp[] = {nullptr, &localhost[0], nullptr};
    if(argc > 1) {
        numConnections = argc - 1;
    } else {
        argv = tmp;
    }
    std::vector<std::thread> connections;
    std::atomic<bool> allSuccess{true};
    for(int connection = 0; connection < numConnections; connection++) {
        connections.push_back(std::thread([connection, &allSuccess, argv](){

            deviceDesc_t deviceDesc;
            strcpy(deviceDesc.name, argv[connection+1]);
            deviceDesc.protocol = X_LINK_TCP_IP;

            printf("Device name: %s\n", deviceDesc.name);

            XLinkHandler_t handler;
            handler.devicePath = deviceDesc.name;
            handler.protocol = deviceDesc.protocol;
            auto connRet = XLinkConnect(&handler);
            printf("Connection %d returned: %s\n", connection, XLinkErrorToStr(connRet));
            if(connRet != X_LINK_SUCCESS) {
                allSuccess = false;
                return;
            }
            auto s = XLinkOpenStream(handler.linkId, "tmp", 1024);
            if(s == INVALID_STREAM_ID){
                printf("Open stream failed...\n");
            } else {
                printf("Open stream OK - conn: %d, name: %s, id: 0x%08X\n", connection, "tmp", s);
                streamPacketDesc_t* p;
                XLinkError_t err = XLinkReadData(s, &p);


                if(err != X_LINK_SUCCESS) {
                    allSuccess = false;
                }
            }

            if(XLinkResetRemote(handler.linkId) != X_LINK_SUCCESS) {
                allSuccess = false;
            }

        }));

    }

    for(auto& conn : connections){
        conn.join();
    }

    if(allSuccess) {
        std::cout << "Success!\n";
        return 0;
    } else {
        std::cout << "RIP!\n";
        return -1;
    }
}

#endif


#ifdef XLINK_TEST_SERVER

// Add shutdownBool
bool shutdownBool = false;
std::mutex shutdownBoolMtx;
std::condition_variable shutdownBoolCv;

// Server
XLinkGlobalHandler_t xlinkGlobalHandler = {};
int main(int argc, const char** argv){

    xlinkGlobalHandler.protocol = X_LINK_TCP_IP;

    // Initialize and suppress XLink logs
    mvLogDefaultLevelSet(MVLOG_ERROR);
    auto status = XLinkInitialize(&xlinkGlobalHandler);
    if(X_LINK_SUCCESS != status) {
        throw std::runtime_error("Couldn't initialize XLink");
    }

    XLinkAddLinkDownCb([](linkId_t linkId) {
        {
            std::unique_lock<std::mutex> l(shutdownBoolMtx);
            shutdownBool = true;
        }
        shutdownBoolCv.notify_all();
    });

    XLinkHandler_t handler;
    std::string serverIp{"127.0.0.1"};
    if(argc > 1) {
        serverIp = std::string(argv[1]);
    }
    handler.devicePath = &serverIp[0];
    handler.protocol = X_LINK_TCP_IP;
    XLinkServer(&handler, "test", X_LINK_BOOTED, X_LINK_MYRIAD_X);

    auto s = XLinkOpenStream(handler.linkId, "tmp", 1024);
    if(s != INVALID_STREAM_ID) {
        uint8_t data[1024] = {};
        if(XLinkWriteData(s, data, sizeof(data)) != X_LINK_SUCCESS) {
            printf("failed.\n");
            return -1;
        }
    } else {
        printf("failed.\n");
        return -1;
    }

    // {
    //     std::unique_lock<std::mutex> l(shutdownBoolMtx);
    //     bool success = shutdownBoolCv.wait_for(l, std::chrono::seconds(3), []() {
    //         return shutdownBool;
    //     });

    //     XLinkResetRemote(handler.linkId);

    //     if(!success) {
    //         printf("timeout waiting for shutdownBool event...\n");
    //         return -1;
    //     }
    // }
    XLinkWaitLink(handler.linkId);


    std::this_thread::sleep_for(std::chrono::seconds(3));

    return 0;
}
#endif