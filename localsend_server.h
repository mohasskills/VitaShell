#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <psp2/io/fcntl.h>

#define MAX_SESSION_FILES   32
#define UPLOAD_BUF_SIZE     (512 * 1024)  // 512 KB per recv — maximises throughput

typedef struct {
    char id[128];
    char file_name[256];
    size_t size;
    char token[128];
    size_t bytes_received;
    int completed;
} SessionFile;

typedef struct {
    char session_id[128];
    char sender_alias[128];
    SessionFile files[MAX_SESSION_FILES];
    int file_count;
    int current_file_index;
    int is_active;
    size_t total_bytes_expected;
    size_t cumulative_bytes_received;
} TransferSession;

// Active upload state - used for async per-frame chunked receiving
typedef struct {
    int        is_active;
    int        client_sock;
    SceUID     file_fd;
    char       safe_name[256];
    size_t     content_length;
    size_t     bytes_written;
    SessionFile *target_sf;
    int        is_chunked;
    char       *initial_body;
    size_t     initial_body_len;

    // Speed tracking
    uint64_t   start_time_us;
    uint64_t   last_speed_time_us;
    size_t     bytes_at_last_speed;
    float      speed_kbps;           // Current transfer speed in KB/s

    SceUID     upload_thread_id;     // Kernel thread handle
    volatile int cancel_requested;
} ActiveUpload;

extern int incoming_request_pending;
extern int pending_client_sock;
extern char pending_sender_alias[128];
extern int pending_file_count;
extern size_t pending_total_size;

extern TransferSession current_session;
extern ActiveUpload    active_upload;
extern char server_status_msg[256];
extern char vita_ip[64];
extern char vita_ssid[64];
extern char save_destination[512];
extern int server_socket;

int is_wifi_connected(void);
extern char vita_fingerprint[64];

void server_init(void);
void server_rebind(void);
void server_poll(void);
void server_term(void);

void server_accept_incoming(void);
void server_reject_incoming(void);

#endif // SERVER_H
