// sender.c — Outbound file transfer (LocalSend HTTP client)
// Implements: discover peers → prepare-upload → upload (chunked, threaded)

#include "localsend_sender.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/net/net.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <sys/time.h>
#include "cJSON/cJSON.h"
#include "main.h"
#include "browser.h"
#include "message_dialog.h"
#include "localsend_dialog.h"
#include "utils.h"

#define SEND_BUF_SIZE (512 * 1024)

// ─── Globals ──────────────────────────────────────────────────────────────────
ActiveSend active_send;
Peer       peer_list[MAX_PEERS];
int        peer_count = 0;

// ─── Internal send queue (set up by sender_start) ────────────────────────────
static char   (*sq_paths)[512] = NULL;
static char   (*sq_names)[256] = NULL;
static size_t *sq_sizes = NULL;
static int    sq_count = 0;
static int    sq_peer_index = -1;

// ─── Peer management ─────────────────────────────────────────────────────────
void sender_add_peer(const char *alias, const char *ip, int port, const char *fingerprint) {
    // Update existing entry by fingerprint
    for (int i = 0; i < peer_count; i++) {
        if (strcmp(peer_list[i].fingerprint, fingerprint) == 0) {
            snprintf(peer_list[i].alias, sizeof(peer_list[i].alias), "%s", alias);
            snprintf(peer_list[i].ip,    sizeof(peer_list[i].ip),    "%s", ip);
            peer_list[i].port = port;
            return;
        }
    }
    if (peer_count >= MAX_PEERS) return;
    Peer *p = &peer_list[peer_count++];
    memset(p, 0, sizeof(Peer));
    snprintf(p->alias,       sizeof(p->alias),       "%s", alias);
    snprintf(p->ip,          sizeof(p->ip),           "%s", ip);
    snprintf(p->fingerprint, sizeof(p->fingerprint),  "%s", fingerprint);
    p->port  = port;
    p->valid = 1;
}

// ─── HTTP helpers (blocking, used only from send thread) ─────────────────────

// recv_timeout_sec = 0 → no recv timeout (blocks indefinitely)
static int tcp_connect(const char *ip, int port) {
    int sock = sceNetSocket("VitaSendClient", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (sock < 0) return sock;

    // (SNDBUF removed to allow accurate cancellation detection)    
    // Generous recv buffer (2MB)
    int rcvbuf = 2 * 1024 * 1024;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    int nodelay = 1;
    sceNetSetsockopt(sock, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &nodelay, sizeof(nodelay));

    SceNetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port   = sceNetHtons((unsigned short)port);
    sceNetInetPton(SCE_NET_AF_INET, ip, &addr.sin_addr);

    // Blocking connect
    int ret = sceNetConnect(sock, (SceNetSockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        sceNetSocketClose(sock);
        return ret;
    }

    // Switch to non-blocking mode AFTER connect. We will handle EAGAIN manually
    // in tcp_send_all and recv_http_response. This avoids Vita setsockopt bugs.
    int nb = 1;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &nb, sizeof(nb));

    return sock;
}


static int tcp_send_all(int sock, const char *buf, size_t len, int epid) {
    size_t sent = 0;
    uint64_t start = sceKernelGetProcessTimeWide();
    while (sent < len) {
        if (epid >= 0) {
            SceNetEpollEvent rev;
            int p = sceNetEpollWait(epid, &rev, 1, 0);
            if (p != 0) {
                // p > 0 (data/early response or EOF), p < 0 (error)
                return -1;
            }
        }

        int n = sceNetSend(sock, buf + sent, len - sent, 0);
        if (n < 0) {
            // 0x80410123 = SCE_NET_ERROR_EAGAIN
            if (n == (int)0x80410123) {
                // 30 second send timeout
                if (sceKernelGetProcessTimeWide() - start > 30000000ULL) return -1;
                sceKernelDelayThread(1000); // 1ms yield
                continue;
            }
            return n;
        }
        if (n == 0) break;
        sent += n;
        start = sceKernelGetProcessTimeWide(); // reset timeout on progress
    }
    return (int)sent;
}

// Receive until the response header AND body are complete on a non-blocking socket.
static size_t recv_http_response(int sock, char *resp_buf, size_t resp_buf_size,
                                 int *status_code, char *resp_preview, int *out_err,
                                 int timeout_sec) {
    size_t total = 0;
    *status_code = 0;
    if (out_err) *out_err = 0;
    if (resp_preview) resp_preview[0] = '\0';

    uint64_t start = sceKernelGetProcessTimeWide();
    int headers_done = 0;
    size_t body_len_expected = 0;
    size_t headers_len = 0;

    while (total < resp_buf_size - 1) {
        int n = sceNetRecv(sock, resp_buf + total, resp_buf_size - 1 - total, 0);
        if (n < 0) {
            if (n == (int)0x80410123) { // EAGAIN
                if (timeout_sec > 0 && (sceKernelGetProcessTimeWide() - start > (uint64_t)timeout_sec * 1000000ULL)) {
                    if (out_err) *out_err = (int)0x8041013C; // ETIMEDOUT
                    break;
                }
                sceKernelDelayThread(1000); // 1ms yield
                continue;
            } else {
                if (out_err) *out_err = n;
                break;
            }
        } else if (n == 0) {
            break; // Connection closed
        }

        total += n;
        resp_buf[total] = '\0';
        start = sceKernelGetProcessTimeWide(); // reset timeout on data

        if (!headers_done) {
            const char *end_of_headers = strstr(resp_buf, "\r\n\r\n");
            if (end_of_headers) {
                headers_done = 1;
                headers_len = (end_of_headers + 4) - resp_buf;
                
                // Parse Content-Length
                const char *cl_pos = strstr(resp_buf, "Content-Length: ");
                if (cl_pos && cl_pos < end_of_headers) {
                    body_len_expected = (size_t)atoi(cl_pos + 16);
                }
            }
        }

        if (headers_done) {
            size_t body_received = total - headers_len;
            // If we have received the full body, or if Content-Length was 0 (or missing) and we don't expect more
            if (body_len_expected > 0 && body_received >= body_len_expected) {
                break;
            }
        }
    }

    // Save first 31 printable bytes for diagnostics
    if (resp_preview && total > 0) {
        size_t plen = total < 31 ? total : 31;
        for (size_t i = 0; i < plen; i++) {
            unsigned char c = (unsigned char)resp_buf[i];
            resp_preview[i] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        resp_preview[plen] = '\0';
    }

    if (total == 0) return 0;

    // Parse status code: find first space in "HTTP/1.x NNN ..."
    const char *p = strstr(resp_buf, "HTTP/");
    if (p) {
        const char *sp = strchr(p, ' ');
        if (sp) *status_code = atoi(sp + 1);
    }
    return total;
}

// Returns pointer to JSON body after headers, or NULL
static const char *extract_body(const char *resp) {
    const char *p = strstr(resp, "\r\n\r\n");
    if (!p) return NULL;
    p += 4;
    
    // The response might use 'transfer-encoding: chunked', which prefixes the
    // body with a hex length (e.g., "6A\r\n{..."). 
    // To bypass this without a full chunked parser, we just scan for the first '{' 
    // since we expect a JSON object. cJSON will stop parsing at the closing '}'.
    const char *json_start = strchr(p, '{');
    return json_start ? json_start : p;
}

// ─── Send thread ──────────────────────────────────────────────────────────────
static int send_thread_func(SceSize args, void *argp) {
    (void)args; (void)argp;

    Peer *peer = &peer_list[sq_peer_index];
    char *io_buf = malloc(SEND_BUF_SIZE);
    if (!io_buf) {
        snprintf(active_send.status_msg, sizeof(active_send.status_msg), "OOM");
        active_send.state = SEND_STATE_ERROR;
        return sceKernelExitDeleteThread(0);
    }

    // ── Step 1: prepare-upload ────────────────────────────────────────────────
    // Build the JSON request body (LocalSend v2 spec)
    cJSON *root = cJSON_CreateObject();
    cJSON *info = cJSON_AddObjectToObject(root, "info");
    cJSON_AddStringToObject(info, "alias",       "VITA");
    cJSON_AddStringToObject(info, "version",     "2.0");
    cJSON_AddStringToObject(info, "deviceModel", "PS Vita");
    cJSON_AddStringToObject(info, "deviceType",  "mobile");
    cJSON_AddStringToObject(info, "fingerprint", "vita-12345");
    cJSON_AddNumberToObject(info, "port",        53317);   // our HTTP server port
    cJSON_AddStringToObject(info, "protocol",    "http"); // we speak plain HTTP
    cJSON_AddBoolToObject  (info, "download",    0);
    cJSON_AddBoolToObject  (info, "announce",    0);

    cJSON *files_obj = cJSON_AddObjectToObject(root, "files");
    for (int i = 0; i < sq_count; i++) {
        char file_id[32];
        snprintf(file_id, sizeof(file_id), "f%d", i);

        cJSON *fi = cJSON_CreateObject();
        cJSON_AddStringToObject(fi, "id",       file_id);
        cJSON_AddStringToObject(fi, "fileName", sq_names[i]);
        cJSON_AddNumberToObject(fi, "size",     (double)sq_sizes[i]);
        cJSON_AddStringToObject(fi, "fileType", "application/octet-stream");
        cJSON_AddItemToObject(files_obj, file_id, fi);
    }

    char *prepare_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    // Connect and POST /api/localsend/v2/prepare-upload
    int prep_sock = tcp_connect(peer->ip, peer->port);
    if (prep_sock < 0) {
        free(prepare_body);
        free(io_buf);
        snprintf(active_send.status_msg, sizeof(active_send.status_msg),
                 "Could not connect to %s. Is LocalSend open?", peer->alias);
        active_send.state = SEND_STATE_ERROR;
        return sceKernelExitDeleteThread(0);
    }

    char http_req[4096];
    size_t body_len = strlen(prepare_body);
    snprintf(http_req, sizeof(http_req),
        "POST /api/localsend/v2/prepare-upload HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n",
        peer->ip, peer->port, (unsigned int)body_len);

    tcp_send_all(prep_sock, http_req, strlen(http_req), -1);
    tcp_send_all(prep_sock, prepare_body, body_len, -1);
    free(prepare_body);

    // Phone holds the connection open while showing the accept dialog.
    // Update status so the user knows to look at their phone.
    snprintf(active_send.status_msg, sizeof(active_send.status_msg),
             "Waiting for accept on %s...", peer->alias);

    char resp_buf[4096];
    memset(resp_buf, 0, sizeof(resp_buf));
    int  status_code  = 0;
    char resp_preview[48] = {0};
    int  recv_err = 0;
    // Wait up to 120 seconds for the user to tap Accept
    size_t resp_bytes = recv_http_response(prep_sock, resp_buf, sizeof(resp_buf),
                                           &status_code, resp_preview, &recv_err, 120);
    sceNetSocketClose(prep_sock);

    if (status_code != 200) {
        free(io_buf);
        if (resp_bytes == 0) {
            if (recv_err < 0) {
                snprintf(active_send.status_msg, sizeof(active_send.status_msg),
                         "Connection lost with %s.", peer->alias);
            } else {
                snprintf(active_send.status_msg, sizeof(active_send.status_msg),
                         "No response from %s", peer->alias);
            }
        } else {
            if (status_code == 403) {
                snprintf(active_send.status_msg, sizeof(active_send.status_msg),
                         "Transfer rejected by %s.", peer->alias);
            } else {
                snprintf(active_send.status_msg, sizeof(active_send.status_msg),
                         "Transfer failed with %s.", peer->alias);
            }
        }
        active_send.state = SEND_STATE_ERROR;
        return sceKernelExitDeleteThread(0);
    }

    // (Debug dumping removed)

    // Parse session_id and per-file tokens
    const char *body = extract_body(resp_buf);
    if (body) {
        cJSON *resp = cJSON_Parse(body);
        if (resp) {
            cJSON *sid = cJSON_GetObjectItemCaseSensitive(resp, "sessionId");
            if (sid && sid->valuestring)
                snprintf(active_send.session_id, sizeof(active_send.session_id),
                         "%s", sid->valuestring);

            cJSON *resp_files = cJSON_GetObjectItemCaseSensitive(resp, "files");
            if (resp_files) {
                for (int i = 0; i < sq_count; i++) {
                    char file_id[32];
                    snprintf(file_id, sizeof(file_id), "f%d", i);
                    cJSON *tok_item = cJSON_GetObjectItemCaseSensitive(resp_files, file_id);
                    if (tok_item && tok_item->valuestring)
                        snprintf(active_send.file_tokens[i], sizeof(active_send.file_tokens[i]),
                                 "%s", tok_item->valuestring);
                }
            }
            cJSON_Delete(resp);
        }
    }

    // ── Step 2: upload each file ──────────────────────────────────────────────
    active_send.state = SEND_STATE_UPLOADING;
    active_send.total_files = sq_count;

    for (int fi = 0; fi < sq_count; fi++) {
        active_send.current_file = fi;
        snprintf(active_send.current_name, sizeof(active_send.current_name),
                 "%.255s", sq_names[fi]);
        snprintf(active_send.status_msg, sizeof(active_send.status_msg),
                 "Sending %.220s (%d/%d)", sq_names[fi], fi + 1, sq_count);

        char file_id[32];
        snprintf(file_id, sizeof(file_id), "f%d", fi);

        // Open local file
        SceUID fd = sceIoOpen(sq_paths[fi], SCE_O_RDONLY, 0);
        if (fd < 0) {
            snprintf(active_send.status_msg, sizeof(active_send.status_msg),
                     "Failed to read file from Vita memory.");
            continue; // skip this file
        }

        // Get actual file size
        SceIoStat fstat;
        memset(&fstat, 0, sizeof(fstat));
        sceIoGetstatByFd(fd, &fstat);
        size_t file_size = (size_t)fstat.st_size;
        if (file_size == 0) file_size = sq_sizes[fi]; // fallback

        // Connect to peer for upload
        int up_sock = tcp_connect(peer->ip, peer->port);
        if (up_sock < 0) {
            sceIoClose(fd);
            snprintf(active_send.status_msg, sizeof(active_send.status_msg),
                     "Lost connection to %s during transfer.", peer->alias);
            active_send.state = SEND_STATE_ERROR;
            break;
        }

        // Send HTTP request headers
        char up_req[1024];
        snprintf(up_req, sizeof(up_req),
            "POST /api/localsend/v2/upload?sessionId=%s&fileId=%s&token=%s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "\r\n",
            active_send.session_id,
            file_id,
            active_send.file_tokens[fi],
            peer->ip, peer->port,
            (unsigned int)file_size);

        tcp_send_all(up_sock, up_req, strlen(up_req), -1);

        // Stream file data
        uint64_t t_start = sceKernelGetProcessTimeWide();
        uint64_t t_speed  = t_start;
        size_t bytes_at_speed = 0;
        active_send.bytes_sent  = 0;
        active_send.bytes_total = file_size;
        active_send.speed_kbps  = 0.0f;

        int epid = sceNetEpollCreate("cancel_poll", 0);
        if (epid >= 0) {
            SceNetEpollEvent ev;
            ev.events = SCE_NET_EPOLLIN;
            ev.data.fd = up_sock;
            sceNetEpollControl(epid, SCE_NET_EPOLL_CTL_ADD, up_sock, &ev);
        }

        while (1) {
            int bytes_read = sceIoRead(fd, io_buf, SEND_BUF_SIZE);
            if (bytes_read <= 0) break;
            
            if (active_send.cancel_requested) {
                sceIoClose(fd);
                sceNetSocketClose(up_sock);
                free(io_buf);

                if (active_send.session_id[0] != '\0') {
                    int cancel_sock = tcp_connect(peer->ip, peer->port);
                    if (cancel_sock >= 0) {
                        char cancel_req[512];
                        snprintf(cancel_req, sizeof(cancel_req),
                            "POST /api/localsend/v2/cancel?sessionId=%s HTTP/1.1\r\n"
                            "Host: %s:%d\r\n"
                            "Content-Length: 0\r\n"
                            "Connection: close\r\n"
                            "\r\n",
                            active_send.session_id, peer->ip, peer->port);
                        tcp_send_all(cancel_sock, cancel_req, strlen(cancel_req), -1);
                        sceNetSocketClose(cancel_sock);
                    }
                }

                snprintf(active_send.status_msg, sizeof(active_send.status_msg), "Transfer cancelled.");
                active_send.state = SEND_STATE_ERROR;
                if (epid >= 0) sceNetEpollDestroy(epid);
                return sceKernelExitDeleteThread(0);
            }

            int sent = tcp_send_all(up_sock, io_buf, (size_t)bytes_read, epid);
            if (sent <= 0) break;

            active_send.bytes_sent += sent;
            active_send.bytes_sent_all_files += sent;

            // Update speed ~every 500 ms
            uint64_t now = sceKernelGetProcessTimeWide();
            uint64_t el  = now - t_speed;
            if (el >= 500000) {
                sceKernelPowerTick(0); // Prevent screen sleep during transfer
                size_t delta = active_send.bytes_sent - bytes_at_speed;
                active_send.speed_kbps = (float)delta / ((float)el / 1000000.0f) / 1024.0f;
                t_speed       = now;
                bytes_at_speed = active_send.bytes_sent;
            }
        }
        if (epid >= 0) sceNetEpollDestroy(epid);
        sceIoClose(fd);

        // Read upload response (ignore body, just drain), 30 sec timeout
        memset(resp_buf, 0, sizeof(resp_buf));
        recv_http_response(up_sock, resp_buf, sizeof(resp_buf), &status_code, NULL, NULL, 30);
        sceNetSocketClose(up_sock);

        if (status_code != 200) {
            snprintf(active_send.status_msg, sizeof(active_send.status_msg), "Cancelled by receiver.");
            active_send.state = SEND_STATE_ERROR;
            free(io_buf);
            return sceKernelExitDeleteThread(0);
        }
    }

    free(io_buf);

    snprintf(active_send.status_msg, sizeof(active_send.status_msg),
             "Sent %d file(s) to %s.", sq_count, peer->alias);
    active_send.state = SEND_STATE_DONE;
    return sceKernelExitDeleteThread(0);
}

// ─── Public API ───────────────────────────────────────────────────────────────

int sender_start(int peer_index,
                 const char *paths[], const char *names[], const size_t *sizes,
                 int file_count) {
    if (active_send.state == SEND_STATE_PREPARING ||
        active_send.state == SEND_STATE_UPLOADING) {
        return -1; // already busy
    }
    if (peer_index < 0 || peer_index >= peer_count) return -2;
    if (file_count <= 0 || file_count > MAX_SEND_FILES) return -3;

    if (sq_paths == NULL) sq_paths = malloc(sizeof(char[512]) * MAX_SEND_FILES);
    if (sq_names == NULL) sq_names = malloc(sizeof(char[256]) * MAX_SEND_FILES);
    if (sq_sizes == NULL) sq_sizes = malloc(sizeof(size_t) * MAX_SEND_FILES);

    char (*old_tokens)[128] = active_send.file_tokens;
    memset(&active_send, 0, sizeof(active_send));
    active_send.file_tokens = old_tokens;
    if (active_send.file_tokens == NULL) active_send.file_tokens = malloc(sizeof(char[128]) * MAX_SEND_FILES);
    
    sq_count      = file_count;
    sq_peer_index = peer_index;

    for (int i = 0; i < file_count; i++) {
        snprintf(sq_paths[i], 512, "%s", paths[i]);
        snprintf(sq_names[i], 256, "%s", names[i]);
        sq_sizes[i] = sizes[i];
        active_send.bytes_total_all_files += sizes[i];
    }

    snprintf(active_send.status_msg, sizeof(active_send.status_msg),
             "Connecting to %s...", peer_list[peer_index].alias);
    active_send.state       = SEND_STATE_PREPARING;
    active_send.total_files = file_count;

    SceUID tid = sceKernelCreateThread(
        "VitaSendOut",
        send_thread_func,
        0x10000100,
        256 * 1024,
        0, 0, NULL
    );
    if (tid < 0) {
        active_send.state = SEND_STATE_ERROR;
        snprintf(active_send.status_msg, sizeof(active_send.status_msg),
                 "Thread create failed: 0x%08X", tid);
        return -4;
    }
    active_send.send_thread_id = tid;
    sceKernelStartThread(tid, 0, NULL);
    return 0;
}

void sender_poll(void) {
    if (active_send.state != SEND_STATE_IDLE && getDialogStep() != DIALOG_STEP_LOCALSEND_SEND) {
        if (getLocalSendDialogStatus() != LOCALSEND_DIALOG_CLOSED) return;
        char short_name[128];
        truncate_string_for_dialog(short_name, active_send.current_name, 250.0f);
        initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, "Sending:\n%s", short_name);
        setDialogStep(DIALOG_STEP_LOCALSEND_SEND);
    }
}

void sender_cancel(void) {
    if (active_send.send_thread_id > 0) {
        active_send.cancel_requested = 1;
        sceKernelWaitThreadEnd(active_send.send_thread_id, NULL, NULL);
    }
    char (*old_tokens)[128] = active_send.file_tokens;
    memset(&active_send, 0, sizeof(active_send));
    active_send.file_tokens = old_tokens;
    active_send.state = SEND_STATE_IDLE;
}
