#include "localsend_discovery.h"
#include "localsend_sender.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include "cJSON/cJSON.h"
#include "localsend_server.h"

#define MULTICAST_IP   "224.0.0.167"
#define MULTICAST_PORT 53317

static int udp_socket  = -1;   // send socket
static int recv_socket = -1;   // listen socket for peer announcements
static SceNetSockaddrIn multicast_addr;
static char* announce_json_str = NULL;

void discovery_init() {
    // ── Send socket ──────────────────────────────────────────────────────────
    udp_socket = sceNetSocket("VitaSendDiscoveryTx", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    if (udp_socket >= 0) {
        int opt = 1;
        sceNetSetsockopt(udp_socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &opt, sizeof(opt));
        sceNetSetsockopt(udp_socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST, &opt, sizeof(opt));
        
        int nb = 1;
        sceNetSetsockopt(udp_socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &nb, sizeof(nb));
    }

    // ── Receive socket (bound to 53317 for both multicast and unicast responses) ─
    recv_socket = sceNetSocket("VitaSendDiscoveryRx", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    if (recv_socket >= 0) {
        int opt = 1;
        sceNetSetsockopt(recv_socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &opt, sizeof(opt));
        sceNetSetsockopt(recv_socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST, &opt, sizeof(opt));

        SceNetSockaddrIn bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family      = SCE_NET_AF_INET;
        bind_addr.sin_port        = sceNetHtons(MULTICAST_PORT);
        bind_addr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
        int bind_ret = sceNetBind(recv_socket, (SceNetSockaddr *)&bind_addr, sizeof(bind_addr));

        if (bind_ret >= 0) {
            // Check if wifi is connected to avoid kernel panic on IP_ADD_MEMBERSHIP
            if (is_wifi_connected()) {
                // Best-effort multicast join — failure is non-fatal; we still receive unicast
                SceNetIpMreq mreq;
                memset(&mreq, 0, sizeof(mreq));
                sceNetInetPton(SCE_NET_AF_INET, MULTICAST_IP, &mreq.imr_multiaddr);
                mreq.imr_interface.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
                if (sceNetSetsockopt(recv_socket, SCE_NET_IPPROTO_IP, SCE_NET_IP_ADD_MEMBERSHIP,
                                     &mreq, sizeof(mreq)) == 0) {
                    extern int multicast_joined;
                    multicast_joined = 1;
                }
            }
        } else {
            // Port 53317 blocked by OS — close and disable UDP reception
            sceNetSocketClose(recv_socket);
            recv_socket = -1;
        }

        // Non-blocking so discovery_poll() doesn't stall the render loop
        if (recv_socket >= 0) {
            int nb = 1;
            sceNetSetsockopt(recv_socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &nb, sizeof(nb));
        }
    }

    // ── Prepare multicast target address ────────────────────────────────────
    memset(&multicast_addr, 0, sizeof(multicast_addr));
    multicast_addr.sin_family = SCE_NET_AF_INET;
    multicast_addr.sin_port   = sceNetHtons(MULTICAST_PORT);
    sceNetInetPton(SCE_NET_AF_INET, MULTICAST_IP, &multicast_addr.sin_addr);

    // ── Build announcement JSON ──────────────────────────────────────────────
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "alias",       "VITA");
    cJSON_AddStringToObject(root, "version",     "2.0");
    cJSON_AddStringToObject(root, "deviceModel", "PS Vita");
    extern char vita_fingerprint[64];
    cJSON_AddStringToObject(root, "deviceType",  "mobile");
    cJSON_AddStringToObject(root, "fingerprint", vita_fingerprint);
    cJSON_AddNumberToObject(root, "port",        53317);
    cJSON_AddStringToObject(root, "protocol",    "http");
    cJSON_AddBoolToObject  (root, "download",    1);
    cJSON_AddBoolToObject  (root, "announce",    1);

    announce_json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
}

extern int server_socket;

void discovery_broadcast() {
    if (is_wifi_connected() && udp_socket >= 0 && server_socket >= 0 && announce_json_str != NULL) {
        // 1. Multicast (Primary)
        sceNetSendto(udp_socket, announce_json_str, strlen(announce_json_str), 0,
               (SceNetSockaddr*)&multicast_addr, sizeof(multicast_addr));

        // 2. Subnet Broadcast Fallback (Safe, doesn't trigger router isolation like 255.255.255.255)
        SceNetCtlInfo ip_info;
        SceNetCtlInfo mask_info;
        memset(&ip_info, 0, sizeof(ip_info));
        memset(&mask_info, 0, sizeof(mask_info));
        
        if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &ip_info) >= 0 &&
            sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_NETMASK, &mask_info) >= 0) {
            
            unsigned int ip = 0;
            unsigned int mask = 0;
            
            if (sceNetInetPton(SCE_NET_AF_INET, ip_info.ip_address, &ip) == 1 &&
                sceNetInetPton(SCE_NET_AF_INET, mask_info.netmask, &mask) == 1) {
                
                unsigned int bcast = ip | (~mask);
                
                SceNetSockaddrIn bcast_addr;
                memset(&bcast_addr, 0, sizeof(bcast_addr));
                bcast_addr.sin_family = SCE_NET_AF_INET;
                bcast_addr.sin_port = sceNetHtons(MULTICAST_PORT);
                bcast_addr.sin_addr.s_addr = bcast;
                
                sceNetSendto(udp_socket, announce_json_str, strlen(announce_json_str), 0,
                             (SceNetSockaddr*)&bcast_addr, sizeof(bcast_addr));
            }
        }
    }
}

int multicast_joined = 0;

// Call each frame to read any incoming peer announcements
void discovery_poll() {
    if (recv_socket < 0) return;

    if (!multicast_joined && is_wifi_connected()) {
        SceNetIpMreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        sceNetInetPton(SCE_NET_AF_INET, MULTICAST_IP, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
        if (sceNetSetsockopt(recv_socket, SCE_NET_IPPROTO_IP, SCE_NET_IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0) {
            multicast_joined = 1;
        }
    }

    char buf[2048];
    SceNetSockaddrIn from_addr;
    unsigned int from_len = sizeof(from_addr);

    // Drain up to 8 datagrams per frame
    for (int iter = 0; iter < 8; iter++) {
        int n = sceNetRecvfrom(recv_socket, buf, sizeof(buf) - 1, 0,
                               (SceNetSockaddr *)&from_addr, &from_len);
        if (n <= 0) break;
        buf[n] = '\0';

        cJSON *root = cJSON_Parse(buf);
        if (!root) continue;

        cJSON *alias_j       = cJSON_GetObjectItemCaseSensitive(root, "alias");
        cJSON *fingerprint_j = cJSON_GetObjectItemCaseSensitive(root, "fingerprint");
        cJSON *port_j        = cJSON_GetObjectItemCaseSensitive(root, "port");

        // Accept announcing peers AND unicast responses (announce:false) —
        // both carry enough info to register the sender.
        if (alias_j && alias_j->valuestring &&
            fingerprint_j && fingerprint_j->valuestring) {

            extern char vita_fingerprint[64];
            // Skip own fingerprint
            if (strcmp(fingerprint_j->valuestring, vita_fingerprint) != 0) {
                char sender_ip[64] = {0};
                sceNetInetNtop(SCE_NET_AF_INET, &from_addr.sin_addr,
                               sender_ip, sizeof(sender_ip));

                int port = 53317;
                if (port_j) {
                    if (cJSON_IsNumber(port_j)) {
                        port = (int)port_j->valuedouble;
                    } else if (cJSON_IsString(port_j)) {
                        port = atoi(port_j->valuestring);
                    }
                }
                
                sender_add_peer(alias_j->valuestring, sender_ip, port,
                                fingerprint_j->valuestring);
            }
        }
        cJSON_Delete(root);
    }
}

void discovery_term() {
    if (udp_socket >= 0) {
        sceNetSocketClose(udp_socket);
        udp_socket = -1;
    }
    if (recv_socket >= 0) {
        sceNetSocketClose(recv_socket);
        recv_socket = -1;
    }
    if (announce_json_str != NULL) {
        free(announce_json_str);
        announce_json_str = NULL;
    }
}
