#include "localsend_server.h"
#include "localsend_sender.h"
#include "localsend_discovery.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include "cJSON/cJSON.h"
#include "file.h"
#include "browser.h"
#include "uncommon_dialog.h"
#include "language.h"
#include "message_dialog.h"

#define SERVER_PORT 53317

int server_socket = -1;
char server_status_msg[256] = "Starting up...";
char save_destination[512] = "ux0:data/vitasend";
char vita_ssid[64] = "";
char vita_fingerprint[64] = "";
TransferSession current_session;
ActiveUpload active_upload;

int incoming_request_pending = 0;
int pending_client_sock = -1;
char pending_sender_alias[128] = {0};
int pending_file_count = 0;
size_t pending_total_size = 0;

int is_wifi_connected(void) {
    int state = 0;
    if (sceNetCtlInetGetState(&state) < 0 || state != SCE_NETCTL_STATE_CONNECTED) {
        return 0;
    }
    return 1;
}

static int get_vita_ip(char *ip_out, size_t max_len) {
    if (!is_wifi_connected()) {
        snprintf(ip_out, max_len, "Not connected to Wi-Fi");
        return 0;
    }
    SceNetCtlInfo info;
    memset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0) {
        snprintf(ip_out, max_len, "%s", info.ip_address);
        
        memset(&info, 0, sizeof(info));
        if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_SSID, &info) >= 0) {
            snprintf(vita_ssid, sizeof(vita_ssid), "%s", info.ssid);
        } else {
            vita_ssid[0] = '\0';
        }
        return 1;
    }
    snprintf(ip_out, max_len, "Unknown IP");
    vita_ssid[0] = '\0';
    return 0;
}

void ensure_save_directory_exists(void) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/vitasend", 0777);
    sceIoMkdir("ux0:data/vitasend/icons", 0777);
    sceIoMkdir(save_destination, 0777);
}

void load_config(void) {
    SceUID fd = sceIoOpen("ux0:data/vitasend/config.txt", SCE_O_RDONLY, 0777);
    if (fd >= 0) {
        char buf[256];
        int len = sceIoRead(fd, buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0';
            // Trim newline
            for (int i = 0; i < len; i++) {
                if (buf[i] == '\r' || buf[i] == '\n') {
                    buf[i] = '\0';
                    break;
                }
            }
            if (buf[0] != '\0') {
                snprintf(save_destination, sizeof(save_destination), "%s", buf);
            }
        }
        sceIoClose(fd);
    }
}

void save_config(void) {
    SceUID fd = sceIoOpen("ux0:data/vitasend/config.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, save_destination, strlen(save_destination));
        sceIoClose(fd);
    }
}

// Helper to set socket to non-blocking
static void set_nonblocking(int sock) {
    int opt = 1;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &opt, sizeof(opt));
}

void server_rebind(void) {
    server_socket = sceNetSocket("VitaSendServer", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (server_socket < 0) {
        snprintf(server_status_msg, sizeof(server_status_msg),
                 "Socket create failed: 0x%08X", server_socket);
        return;
    }

    int opt = 1;
    sceNetSetsockopt(server_socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &opt, sizeof(opt));

    // Large receive buffer — lets TCP keep more data in-flight, boosting throughput
    int rcvbuf = 256 * 1024;
    sceNetSetsockopt(server_socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    SceNetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = SCE_NET_AF_INET;
    addr.sin_port        = sceNetHtons(SERVER_PORT);
    addr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);

    int bind_res = sceNetBind(server_socket, (SceNetSockaddr *)&addr, sizeof(addr));
    if (bind_res < 0) {
        snprintf(server_status_msg, sizeof(server_status_msg),
                 "Bind error: 0x%08X", bind_res);
        sceNetSocketClose(server_socket);
        server_socket = -1;
        return;
    }

    int listen_res = sceNetListen(server_socket, 5);
    if (listen_res < 0) {
        snprintf(server_status_msg, sizeof(server_status_msg),
                 "Listen error: 0x%08X", listen_res);
        sceNetSocketClose(server_socket);
        server_socket = -1;
        return;
    }

    set_nonblocking(server_socket);
    snprintf(server_status_msg, sizeof(server_status_msg),
             "Ready: http://%s:%d", vita_ip, SERVER_PORT);
}

void server_init(void) {
    memset(&current_session, 0, sizeof(current_session));
    load_config();
    ensure_save_directory_exists();

    if (vita_fingerprint[0] == '\0') {
        SceNetEtherAddr mac;
        if (sceNetGetMacAddress(&mac, 0) == 0) {
            snprintf(vita_fingerprint, sizeof(vita_fingerprint), "vita-%02x%02x%02x%02x%02x%02x", 
                     mac.data[0], mac.data[1], mac.data[2], mac.data[3], mac.data[4], mac.data[5]);
        } else {
            snprintf(vita_fingerprint, sizeof(vita_fingerprint), "vita-%08X", (unsigned int)sceKernelGetProcessTimeLow());
        }
    }

    get_vita_ip(vita_ip, sizeof(vita_ip));

    server_rebind();
    discovery_init();
}


static void send_http_response(int client_sock, int status_code, const char* status_text, const char* content_type, const char* body) {
    char header[1024];
    unsigned int body_len = (unsigned int)(body ? strlen(body) : 0);
    snprintf(header, sizeof(header), 
        "HTTP/1.1 %d %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: *\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n", 
        status_code, status_text,
        content_type ? content_type : "application/json", 
        body_len);

    sceNetSend(client_sock, header, strlen(header), 0);
    if (body && body_len > 0) {
        sceNetSend(client_sock, body, body_len, 0);
    }
}

static void get_query_param(const char *url, const char *param, char *out, size_t max_len) {
    out[0] = '\0';
    char search[128];
    snprintf(search, sizeof(search), "%s=", param);
    
    const char *q = strchr(url, '?');
    if (!q) return;
    
    const char *pos = strstr(q, search);
    if (!pos) return;
    
    pos += strlen(search);
    size_t i = 0;
    while (*pos != '\0' && *pos != '&' && *pos != ' ' && i < max_len - 1) {
        out[i++] = *pos++;
    }
    out[i] = '\0';
}

static void ensure_parent_directories(const char *filepath) {
    char temp[512];
    snprintf(temp, sizeof(temp), "%s", filepath);
    
    char *p = temp;
    // Skip the drive letter or partition (e.g., "ux0:")
    char *colon = strchr(p, ':');
    if (colon) {
        p = colon + 1;
        if (*p == '/') p++;
    }

    while (*p) {
        if (*p == '/') {
            *p = '\0'; // temporarily terminate the string
            sceIoMkdir(temp, 0777); // attempt to create directory
            *p = '/';  // restore slash
        }
        p++;
    }
}

static void sanitize_filename(const char *input, char *output, size_t max_len) {
    size_t out_idx = 0;

    // Skip leading slashes to prevent absolute paths
    while (*input == '/' || *input == '\\') {
        input++;
    }

    for (size_t i = 0; input[i] != '\0' && out_idx < max_len - 1; i++) {
        char c = input[i];

        // Replace '..' with '__' to prevent directory traversal
        if (c == '.' && input[i+1] == '.') {
            output[out_idx++] = '_';
            if (out_idx < max_len - 1) {
                output[out_idx++] = '_';
            }
            i++; // skip the second dot
            continue;
        }

        // Unify slashes
        if (c == '\\') c = '/';

        // Allow '/' but sanitize other invalid characters
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
        output[out_idx++] = c;
    }
    output[out_idx] = '\0';
    if (out_idx == 0) {
        snprintf(output, max_len, "received_file");
    }
}

static const char* find_header(const char *headers, const char *header_name) {
    char lower_name[64];
    size_t len = strlen(header_name);
    for (size_t i = 0; i < len && i < sizeof(lower_name) - 1; i++) {
        lower_name[i] = (header_name[i] >= 'A' && header_name[i] <= 'Z') ? (header_name[i] + 32) : header_name[i];
    }
    lower_name[len] = '\0';

    const char *line = headers;
    while (line && *line != '\0') {
        size_t i = 0;
        int match = 1;
        for (; i < len; i++) {
            char c = line[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != lower_name[i]) { match = 0; break; }
        }
        if (match && line[len] == ':') {
            const char *val = line + len + 1;
            while (*val == ' ' || *val == '\t') val++;
            return val;
        }
        line = strstr(line, "\r\n");
        if (line) line += 2;
    }
    return NULL;
}

// handle_register: respond with our device info AND register the caller as a send peer.
// client_ip is the remote IP extracted from the accept() address.
// body/body_len is any HTTP body (LocalSend POSTs its own device info here).
static void handle_register(int client_sock, const char *client_ip,
                            const char *body, size_t body_len) {
    // ── Parse caller's device info (if they sent a body) ─────────────────────
    char caller_alias[128]       = "LocalSend Device";
    char caller_fingerprint[128] = {0};
    int  caller_port             = 53317; // LocalSend default

    if (body && body_len > 0) {
        cJSON *req = cJSON_Parse(body);
        if (req) {
            cJSON *a = cJSON_GetObjectItemCaseSensitive(req, "alias");
            cJSON *f = cJSON_GetObjectItemCaseSensitive(req, "fingerprint");
            cJSON *p = cJSON_GetObjectItemCaseSensitive(req, "port");
            if (a && a->valuestring)
                snprintf(caller_alias, sizeof(caller_alias), "%.127s", a->valuestring);
            if (f && f->valuestring)
                snprintf(caller_fingerprint, sizeof(caller_fingerprint), "%.127s", f->valuestring);
            if (p && cJSON_IsNumber(p))
                caller_port = (int)p->valuedouble;
            cJSON_Delete(req);
        }
    }

    // Fall back to IP as fingerprint if the caller didn't send one
    if (caller_fingerprint[0] == '\0' && client_ip && client_ip[0])
        snprintf(caller_fingerprint, sizeof(caller_fingerprint), "ip-%s", client_ip);

    // Add (or refresh) peer entry — safe to call even with empty strings
    if (client_ip && client_ip[0])
        sender_add_peer(caller_alias, client_ip, caller_port, caller_fingerprint);

    // ── Respond with our own device info ─────────────────────────────────────
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "alias",       "VITA");
    cJSON_AddStringToObject(root, "version",     "2.0");
    cJSON_AddStringToObject(root, "deviceModel", "PS Vita");
    cJSON_AddStringToObject(root, "deviceType",  "mobile");
    cJSON_AddStringToObject(root, "fingerprint", vita_fingerprint);
    cJSON_AddBoolToObject  (root, "download",    1);

    char *json_str = cJSON_PrintUnformatted(root);
    send_http_response(client_sock, 200, "OK", "application/json", json_str);
    free(json_str);
    cJSON_Delete(root);
}

static int handle_prepare_upload(int client_sock, const char* body) {
    cJSON *req = cJSON_Parse(body);
    if (!req) {
        send_http_response(client_sock, 400, "Bad Request", "application/json", "{\"message\":\"Invalid JSON\"}");
        return 0;
    }

    memset(&current_session, 0, sizeof(current_session));
    pending_total_size = 0;
    snprintf(current_session.session_id, sizeof(current_session.session_id), "vita-sess-%u", (unsigned int)sceKernelGetProcessTimeLow());
    
    cJSON *info = cJSON_GetObjectItemCaseSensitive(req, "info");
    if (info) {
        cJSON *alias = cJSON_GetObjectItemCaseSensitive(info, "alias");
        if (alias && alias->valuestring) {
            snprintf(current_session.sender_alias, sizeof(current_session.sender_alias), "%s", alias->valuestring);
        }
    }
    if (strlen(current_session.sender_alias) == 0) {
        strcpy(current_session.sender_alias, "Remote Device");
    }

    cJSON *req_files = cJSON_GetObjectItemCaseSensitive(req, "files");
    if (req_files) {
        cJSON *file_item = NULL;
        cJSON_ArrayForEach(file_item, req_files) {
            if (current_session.file_count >= MAX_SESSION_FILES) break;
            
            SessionFile *sf = &current_session.files[current_session.file_count];
            memset(sf, 0, sizeof(SessionFile));
            
            const char *file_id = file_item->string;
            cJSON *id_obj = cJSON_GetObjectItemCaseSensitive(file_item, "id");
            if (id_obj && id_obj->valuestring) {
                file_id = id_obj->valuestring;
            }

            cJSON *name_obj = cJSON_GetObjectItemCaseSensitive(file_item, "fileName");
            cJSON *size_obj = cJSON_GetObjectItemCaseSensitive(file_item, "size");

            snprintf(sf->id, sizeof(sf->id), "%s", file_id ? file_id : "0");
            if (name_obj && name_obj->valuestring) {
                snprintf(sf->file_name, sizeof(sf->file_name), "%s", name_obj->valuestring);
            } else {
                snprintf(sf->file_name, sizeof(sf->file_name), "file_%d", current_session.file_count);
            }

            if (size_obj) {
                sf->size = (size_t)size_obj->valuedouble;
                pending_total_size += sf->size;
                current_session.total_bytes_expected += sf->size;
            }

            snprintf(sf->token, sizeof(sf->token), "tok-%u-%d", (unsigned int)sceKernelGetProcessTimeLow(), current_session.file_count);
            sf->bytes_received = 0;
            sf->completed = 0;

            current_session.file_count++;
        }
    }

    current_session.is_active = 0; // Not active until accepted
    current_session.current_file_index = 0;

    pending_client_sock = client_sock;
    snprintf(pending_sender_alias, sizeof(pending_sender_alias), "%s", current_session.sender_alias);
    pending_file_count = current_session.file_count;
    incoming_request_pending = 1;

    cJSON_Delete(req);
    
    // DO NOT auto-accept. The main UI thread will poll `incoming_request_pending`
    // and present a dialog to the user.
    return 1; // Indicate we took ownership of the socket
}

void server_accept_incoming(void) {
    if (!incoming_request_pending || pending_client_sock < 0) return;

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "sessionId", current_session.session_id);
    cJSON *resp_files = cJSON_AddObjectToObject(response, "files");

    for (int i = 0; i < current_session.file_count; i++) {
        cJSON_AddStringToObject(resp_files, current_session.files[i].id, current_session.files[i].token);
    }

    current_session.is_active = 1;

    snprintf(server_status_msg, sizeof(server_status_msg), "Receiving %d file(s) from %s", 
             current_session.file_count, current_session.sender_alias);

    char *json_str = cJSON_PrintUnformatted(response);
    send_http_response(pending_client_sock, 200, "OK", "application/json", json_str);

    free(json_str);
    cJSON_Delete(response);
    
    sceNetSocketClose(pending_client_sock);
    pending_client_sock = -1;
    incoming_request_pending = 0;
}

void server_reject_incoming(void) {
    if (!incoming_request_pending || pending_client_sock < 0) return;
    send_http_response(pending_client_sock, 403, "Forbidden", "application/json", "{\"message\":\"User rejected\"}");
    sceNetSocketClose(pending_client_sock);
    pending_client_sock = -1;
    incoming_request_pending = 0;
    memset(&current_session, 0, sizeof(current_session));
}

// ─── Upload kernel thread ──────────────────────────────────────────────────
// Runs independently of the render loop; receives data as fast as the network
// delivers it (no vsync bottleneck).  Writes in 128 KB chunks straight to disk.

static int upload_thread_func(SceSize args, void *argp) {
    (void)args; (void)argp;

    char *buf = malloc(UPLOAD_BUF_SIZE);
    if (!buf) {
        active_upload.is_active = 0;
        return sceKernelExitDeleteThread(0);
    }

    // State for chunked transfer encoding
    int chunk_state = 0; // 0=read_size, 1=read_ext, 2=read_data, 3=read_cr, 4=read_lf, 5=read_lf_after_data
    size_t chunk_bytes_left = 0;
    
    // Process initial_body first, then switch to reading from socket
    char *current_data = active_upload.initial_body;
    size_t current_len = active_upload.initial_body_len;
    int socket_eof = 0;

    while (1) {
        if (active_upload.cancel_requested) {
            break;
        }

        if (current_len == 0) {
            if (socket_eof) break;
            
            size_t to_recv = UPLOAD_BUF_SIZE;
            if (!active_upload.is_chunked && active_upload.content_length > 0) {
                size_t remaining = active_upload.content_length - active_upload.bytes_written;
                if (remaining == 0) break;
                if (remaining < to_recv) to_recv = remaining;
            }

            int bytes = sceNetRecv(active_upload.client_sock, buf, to_recv, 0);
            if (bytes <= 0) {
                socket_eof = 1;
                break;
            }
            current_data = buf;
            current_len = bytes;
        }

        if (active_upload.cancel_requested) break;

        if (!active_upload.is_chunked) {
            // Normal (non-chunked) transfer
            sceIoWrite(active_upload.file_fd, current_data, current_len);
            active_upload.bytes_written += current_len;
            current_session.cumulative_bytes_received += current_len;
            current_len = 0;
        } else {
            // Chunked decoding
            size_t i = 0;
            while (i < current_len) {
                char c = current_data[i];
                if (chunk_state == 0) { // read_size
                    if (c >= '0' && c <= '9') {
                        chunk_bytes_left = chunk_bytes_left * 16 + (c - '0');
                        i++;
                    } else if (c >= 'a' && c <= 'f') {
                        chunk_bytes_left = chunk_bytes_left * 16 + (c - 'a' + 10);
                        i++;
                    } else if (c >= 'A' && c <= 'F') {
                        chunk_bytes_left = chunk_bytes_left * 16 + (c - 'A' + 10);
                        i++;
                    } else if (c == ';') {
                        chunk_state = 1; // extension
                        i++;
                    } else if (c == '\r') {
                        chunk_state = 4; // wait for LF before data
                        i++;
                    } else if (c == '\n') {
                        chunk_state = 2; // start data
                        if (chunk_bytes_left == 0) { socket_eof = 1; break; }
                        i++;
                    } else {
                        i++;
                    }
                } else if (chunk_state == 1) { // read_ext
                    if (c == '\r') chunk_state = 4;
                    else if (c == '\n') {
                        chunk_state = 2;
                        if (chunk_bytes_left == 0) { socket_eof = 1; break; }
                    }
                    i++;
                } else if (chunk_state == 4) { // wait for LF
                    if (c == '\n') {
                        chunk_state = 2;
                        if (chunk_bytes_left == 0) {
                            socket_eof = 1; // End of chunked stream
                            break;
                        }
                    }
                    i++;
                } else if (chunk_state == 2) { // read_data
                    size_t avail = current_len - i;
                    size_t to_write = avail < chunk_bytes_left ? avail : chunk_bytes_left;
                    sceIoWrite(active_upload.file_fd, current_data + i, to_write);
                    active_upload.bytes_written += to_write;
                    current_session.cumulative_bytes_received += to_write;
                    chunk_bytes_left -= to_write;
                    i += to_write;

                    if (chunk_bytes_left == 0) {
                        chunk_state = 3; // wait for CR after data
                    }
                } else if (chunk_state == 3) { // wait for CR after data
                    if (c == '\r') chunk_state = 5;
                    else if (c == '\n') { chunk_state = 0; chunk_bytes_left = 0; }
                    i++;
                } else if (chunk_state == 5) { // wait for LF after data
                    if (c == '\n') { chunk_state = 0; chunk_bytes_left = 0; }
                    i++;
                }
            }
            if (socket_eof) break; // Finished parsing all chunks
            current_len = 0; // processed all
        }

        if (active_upload.target_sf)
            active_upload.target_sf->bytes_received = active_upload.bytes_written;

        // Speed: update every ~500ms
        uint64_t now = sceKernelGetProcessTimeWide();
        uint64_t elapsed_us = now - active_upload.last_speed_time_us;
        if (elapsed_us >= 500000) {
            sceKernelPowerTick(0); // Prevent screen sleep during transfer
            size_t delta = active_upload.bytes_written - active_upload.bytes_at_last_speed;
            float elapsed_s = (float)elapsed_us / 1000000.0f;
            active_upload.speed_kbps = (float)delta / elapsed_s / 1024.0f;
            active_upload.last_speed_time_us  = now;
            active_upload.bytes_at_last_speed = active_upload.bytes_written;
        }
    }

    if (!active_upload.is_chunked && active_upload.content_length > 0 && 
        active_upload.bytes_written < active_upload.content_length) {
        active_upload.cancel_requested = 1;
    }

    free(buf);
    if (active_upload.initial_body) {
        free(active_upload.initial_body);
        active_upload.initial_body = NULL;
    }

    // Finalize
    sceIoClose(active_upload.file_fd);
    active_upload.file_fd = -1;

    if (active_upload.target_sf) {
        active_upload.target_sf->bytes_received = active_upload.bytes_written;
        active_upload.target_sf->completed = active_upload.cancel_requested ? 0 : 1;
    }

    if (active_upload.cancel_requested) {
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", save_destination, active_upload.safe_name);
        sceIoRemove(filepath);
        // We should send a failure response, though socket might be dead
        send_http_response(active_upload.client_sock, 403, "Forbidden", "application/json", "{\"message\":\"Canceled by receiver\"}");
    } else {
        send_http_response(active_upload.client_sock, 200, "OK", "application/json", "{}");
    }
    
    sceNetSocketClose(active_upload.client_sock);
    active_upload.client_sock = -1;

    // Update session status
    int all_done = 1;
    for (int i = 0; i < current_session.file_count; i++) {
        if (!current_session.files[i].completed) { all_done = 0; break; }
    }
    if (all_done && !active_upload.cancel_requested) {
        snprintf(server_status_msg, sizeof(server_status_msg),
                 "Done! Received %d file(s) from %s",
                 current_session.file_count, current_session.sender_alias);
        
        // Debug log to tell the user EXACTLY where it went
        char logpath[512] = "ux0:data/vitasend_last_received.txt";
        SceUID logfd = sceIoOpen(logpath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (logfd >= 0) {
            char logmsg[1024];
            snprintf(logmsg, sizeof(logmsg), "Last file was saved to: %s/%s\nFile size: %d bytes\n", save_destination, active_upload.safe_name, (int)active_upload.bytes_written);
            sceIoWrite(logfd, logmsg, strlen(logmsg));
            sceIoClose(logfd);
        }

        current_session.is_active = 0;
    } else if (active_upload.cancel_requested) {
        snprintf(server_status_msg, sizeof(server_status_msg), "Cancelled by sender");
        current_session.is_active = 0;
    } else {
        snprintf(server_status_msg, sizeof(server_status_msg),
                 "Saved: %.200s", active_upload.safe_name);
    }

    active_upload.is_active = 0;
    return sceKernelExitDeleteThread(0);
}

// ─── start_upload ──────────────────────────────────────────────────────────
// Called once when an upload request arrives. Opens the file, writes any
// pipelined data, then spawns upload_thread_func and returns immediately.

static void start_upload(int client_sock, const char *url_path, const char *headers,
                         const char *initial_body, size_t initial_body_len) {
    // Generous recv buffer (2MB) for maximum upload throughput
    int rcvbuf = 2 * 1024 * 1024;
    sceNetSetsockopt(client_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    
    // Generous send buffer (2MB) just in case
    int sndbuf = 2 * 1024 * 1024;
    sceNetSetsockopt(client_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    int nodelay = 1;
    sceNetSetsockopt(client_sock, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &nodelay, sizeof(nodelay));
    if (active_upload.is_active) {
        send_http_response(client_sock, 503, "Service Unavailable", "application/json",
                           "{\"message\":\"Upload already in progress\"}");
        sceNetSocketClose(client_sock);
        return;
    }

    char file_id[128] = {0};
    char token[128]   = {0};
    get_query_param(url_path, "fileId", file_id, sizeof(file_id));
    get_query_param(url_path, "token",  token,   sizeof(token));

    SessionFile *target_sf = NULL;
    for (int i = 0; i < current_session.file_count; i++) {
        if (strcmp(current_session.files[i].id, file_id) == 0) {
            target_sf = &current_session.files[i];
            break;
        }
    }
    if (!target_sf && current_session.file_count > 0)
        target_sf = &current_session.files[0];
    if (!target_sf) {
        send_http_response(client_sock, 403, "Forbidden", "application/json",
                           "{\"message\":\"Invalid fileId\"}");
        sceNetSocketClose(client_sock);
        return;
    }

    size_t content_length = target_sf->size;
    const char *cl_header = find_header(headers, "Content-Length");
    if (cl_header) content_length = (size_t)strtoul(cl_header, NULL, 10);
    
    int is_chunked = 0;
    const char *te_header = find_header(headers, "Transfer-Encoding");
    if (te_header && strstr(te_header, "chunked") != NULL) {
        is_chunked = 1;
    }

    ensure_save_directory_exists();

    // Build the state (memset first so safe_name gets stored correctly)
    memset(&active_upload, 0, sizeof(active_upload));
    sanitize_filename(target_sf->file_name, active_upload.safe_name, sizeof(active_upload.safe_name));

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", save_destination, active_upload.safe_name);

    SceIoStat stat;
    int counter = 1;
    
    char base_name[256];
    char ext[64] = "";
    strcpy(base_name, active_upload.safe_name);
    char *last_slash = strrchr(base_name, '/');
    char *dot = strrchr(last_slash ? last_slash : base_name, '.');
    if (dot && dot != base_name && dot != (last_slash ? last_slash + 1 : base_name)) {
        strcpy(ext, dot);
        *dot = '\0';
    }

    while (sceIoGetstat(filepath, &stat) >= 0) {
        char new_name[256];
        snprintf(new_name, sizeof(new_name), "%s(%d)%s", base_name, counter, ext);
        snprintf(filepath, sizeof(filepath), "%s/%s", save_destination, new_name);
        strcpy(active_upload.safe_name, new_name);
        counter++;
    }

    ensure_parent_directories(filepath);
    SceUID fd = sceIoOpen(filepath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        send_http_response(client_sock, 500, "Internal Server Error", "application/json",
                           "{\"message\":\"Failed to open file\"}");
        sceNetSocketClose(client_sock);
        return;
    }

    // Pass initial body to the upload thread instead of writing directly
    char *initial_body_copy = NULL;
    if (initial_body_len > 0) {
        initial_body_copy = malloc(initial_body_len);
        if (initial_body_copy) {
            memcpy(initial_body_copy, initial_body, initial_body_len);
        } else {
            initial_body_len = 0;
        }
    }

    active_upload.client_sock         = client_sock;
    active_upload.file_fd             = fd;
    active_upload.content_length      = content_length;
    active_upload.bytes_written       = 0;
    active_upload.target_sf           = target_sf;
    active_upload.is_chunked          = is_chunked;
    active_upload.initial_body        = initial_body_copy;
    active_upload.initial_body_len    = initial_body_len;
    active_upload.start_time_us       = sceKernelGetProcessTimeWide();
    active_upload.last_speed_time_us  = active_upload.start_time_us;
    active_upload.bytes_at_last_speed = 0;
    active_upload.speed_kbps          = 0.0f;
    active_upload.upload_thread_id    = -1;

    snprintf(server_status_msg, sizeof(server_status_msg),
             "Receiving: %.220s", active_upload.safe_name);

    // Spawn the upload thread — it runs at the same priority as the main thread
    // but blocks on sceNetRecv, yielding CPU back to the render loop automatically.
    SceUID tid = sceKernelCreateThread(
        "VitaSendUpload",
        upload_thread_func,
        0x10000100,      // priority (same as user main thread)
        256 * 1024,      // 256 KB stack (needs room for the 128 KB recv buf on heap)
        0, 0, NULL
    );

    if (tid >= 0) {
        active_upload.upload_thread_id = tid;
        active_upload.is_active = 1;
        sceKernelStartThread(tid, 0, NULL);
    } else {
        // Thread creation failed — clean up gracefully
        sceIoClose(fd);
        send_http_response(client_sock, 500, "Internal Server Error", "application/json",
                           "{\"message\":\"Thread creation failed\"}");
        sceNetSocketClose(client_sock);
        snprintf(server_status_msg, sizeof(server_status_msg),
                 "Thread error: 0x%08X", tid);
    }
}


static void handle_client(int client_sock, const char *client_ip) {
    #define RECV_HEADER_BUF 4096
    char *buf = malloc(RECV_HEADER_BUF);
    if (!buf) {
        sceNetSocketClose(client_sock);
        return;
    }
    memset(buf, 0, RECV_HEADER_BUF);

    int total_bytes = 0;
    char *header_end = NULL;

    while (total_bytes < RECV_HEADER_BUF - 1) {
        int bytes = sceNetRecv(client_sock, buf + total_bytes, RECV_HEADER_BUF - 1 - total_bytes, 0);
        if (bytes <= 0) break;
        total_bytes += bytes;
        buf[total_bytes] = '\0';

        header_end = strstr(buf, "\r\n\r\n");
        if (header_end) break;
    }

    if (!header_end) {
        free(buf);
        sceNetSocketClose(client_sock);
        return;
    }

    size_t header_len = (header_end + 4) - buf;
    const char *initial_body = buf + header_len;
    size_t initial_body_len = total_bytes - header_len;

    char first_line[512] = {0};
    const char *line_end = strstr(buf, "\r\n");
    if (line_end) {
        size_t fl_len = line_end - buf;
        if (fl_len >= sizeof(first_line)) fl_len = sizeof(first_line) - 1;
        strncpy(first_line, buf, fl_len);
    } else {
        strncpy(first_line, buf, sizeof(first_line) - 1);
    }

    if (strncmp(first_line, "OPTIONS", 7) == 0) {
        send_http_response(client_sock, 204, "No Content", "text/plain", NULL);
        free(buf);
        sceNetSocketClose(client_sock);
        return;
    }

    if (strstr(first_line, "/register") != NULL ||
        strstr(first_line, "/info") != NULL) {
        // Read body if present (POST with caller device info)
        size_t reg_body_len = 0;
        char *reg_body = NULL;
        const char *reg_cl = find_header(buf, "Content-Length");
        if (reg_cl) reg_body_len = (size_t)strtoul(reg_cl, NULL, 10);
        if (reg_body_len > 0 && reg_body_len <= 4096) {
            reg_body = malloc(reg_body_len + 1);
            if (reg_body) {
                memset(reg_body, 0, reg_body_len + 1);
                size_t already = initial_body_len < reg_body_len ? initial_body_len : reg_body_len;
                memcpy(reg_body, initial_body, already);
                while (already < reg_body_len) {
                    int n = sceNetRecv(client_sock, reg_body + already, reg_body_len - already, 0);
                    if (n <= 0) break;
                    already += n;
                }
                reg_body[already] = '\0';
                reg_body_len = already;
            }
        } else {
            // No content-length; use whatever arrived with the headers
            reg_body = NULL;
            reg_body_len = 0;
        }
        handle_register(client_sock, client_ip,
                        reg_body ? reg_body : initial_body,
                        reg_body ? reg_body_len : initial_body_len);
        free(reg_body); // safe even if NULL
    } else if (strstr(first_line, "/prepare-upload") != NULL) {
        size_t content_len = 0;
        const char *cl = find_header(buf, "Content-Length");
        if (cl) content_len = (size_t)strtoul(cl, NULL, 10);

        char *full_body = malloc(content_len + 1);
        if (full_body) {
            memset(full_body, 0, content_len + 1);
            size_t body_read = initial_body_len;
            if (body_read > content_len) body_read = content_len;
            memcpy(full_body, initial_body, body_read);

            while (body_read < content_len) {
                int bytes = sceNetRecv(client_sock, full_body + body_read, content_len - body_read, 0);
                if (bytes <= 0) break;
                body_read += bytes;
            }
            full_body[body_read] = '\0';
            int kept = handle_prepare_upload(client_sock, full_body);
            free(full_body);
            if (kept) {
                free(buf);
                return;
            }
        } else {
            send_http_response(client_sock, 500, "Internal Server Error", "application/json", "{\"message\":\"Out of memory\"}");
        }
    } else if (strstr(first_line, "/upload") != NULL) {
        char url_path[256] = {0};
        const char *sp1 = strchr(first_line, ' ');
        if (sp1) {
            sp1++;
            const char *sp2 = strchr(sp1, ' ');
            if (sp2) {
                size_t path_len = sp2 - sp1;
                if (path_len >= sizeof(url_path)) path_len = sizeof(url_path) - 1;
                strncpy(url_path, sp1, path_len);
            }
        }
        // start_upload takes ownership of client_sock — do NOT close it here
        start_upload(client_sock, url_path, buf, initial_body, initial_body_len);
        free(buf);
        return;  // early return — socket managed by active_upload
    } else if (strstr(first_line, "/cancel") != NULL) {
        char url_path[256] = {0};
        const char *sp1 = strchr(first_line, ' ');
        if (sp1) {
            sp1++;
            const char *sp2 = strchr(sp1, ' ');
            if (sp2) {
                size_t path_len = sp2 - sp1;
                if (path_len >= sizeof(url_path)) path_len = sizeof(url_path) - 1;
                strncpy(url_path, sp1, path_len);
            }
        }
        
        char session_id[128] = {0};
        get_query_param(url_path, "sessionId", session_id, sizeof(session_id));
        
        // Check if it's cancelling the Vita's active SEND operation
        if (active_send.state == SEND_STATE_UPLOADING || active_send.state == SEND_STATE_PREPARING) {
            if (strcmp(session_id, active_send.session_id) == 0) {
                active_send.cancel_requested = 1;
            }
        }
        
        // Also check if it's cancelling an active RECEIVE operation
        if (current_session.is_active && strcmp(session_id, current_session.session_id) == 0) {
            active_upload.cancel_requested = 1;
        }

        send_http_response(client_sock, 200, "OK", "application/json", "{}");
    } else {
        send_http_response(client_sock, 404, "Not Found", "application/json", "{\"message\":\"Endpoint not found\"}");
    }

    free(buf);
    sceNetSocketClose(client_sock);
}

void server_poll(void) {
    static uint64_t last_ip_check = 0;
    uint64_t current_time = sceKernelGetProcessTimeWide();
    if (current_time - last_ip_check > 2000000) {
        last_ip_check = current_time;
        get_vita_ip(vita_ip, sizeof(vita_ip));
    }

    // Upload thread is running — don't accept new connections until it finishes.
    // The thread itself handles all recv/write/respond; we just yield here.
    if (active_upload.is_active) {
        sceKernelDelayThread(1000); // 1 ms yield so render loop isn't spinning
        return;
    }

    if (server_socket < 0) {
        static uint64_t last_retry_time = 0;
        uint64_t current_time = sceKernelGetProcessTimeWide();
        if (current_time - last_retry_time > 1000000) {
            last_retry_time = current_time;
            server_rebind();
        }
        return;
    }

    SceNetSockaddrIn client_addr;
    unsigned int client_len = sizeof(client_addr);

    int client_sock = sceNetAccept(server_socket, (SceNetSockaddr *)&client_addr, &client_len);
    if (client_sock >= 0) {
        // Blocking mode for client
        int nb = 0;
        sceNetSetsockopt(client_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &nb, sizeof(nb));

        // 256 KB receive buffer — critical for throughput over Wi-Fi
        int rcvbuf = 256 * 1024;
        sceNetSetsockopt(client_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        // Disable Nagle's algorithm — sends ACKs immediately instead of batching,
        // which prevents Android from stalling while waiting for ACKs.
        int nodelay = 1;
        sceNetSetsockopt(client_sock, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &nodelay, sizeof(nodelay));

        // Extract client IP for peer auto-registration
        char client_ip[64] = {0};
        sceNetInetNtop(SCE_NET_AF_INET, &client_addr.sin_addr,
                       client_ip, sizeof(client_ip));

        handle_client(client_sock, client_ip);
    }
}

void server_term(void) {
    if (server_socket >= 0) {
        sceNetSocketClose(server_socket);
        server_socket = -1;
    }
    if (active_upload.is_active) {
        active_upload.cancel_requested = 1;
    }
    discovery_term();
}
