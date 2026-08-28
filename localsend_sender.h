#ifndef SENDER_H
#define SENDER_H

#include <stddef.h>
#include <stdint.h>
#include <psp2/types.h>

// ─── Peer / Discovery ─────────────────────────────────────────────────────────
#define MAX_PEERS 16

typedef struct {
    char alias[128];
    char ip[64];
    int  port;
    char fingerprint[128];
    int  valid;
} Peer;

// ─── File queue ──────────────────────────────────────────────────────────────
typedef struct {
    char path[512];      // full vita path, e.g. "ux0:data/foo.txt"
    char name[256];      // basename
    size_t size;
    int  selected;       // 1 = queued for sending
} SendFile;

// ─── Outbound transfer state ──────────────────────────────────────────────────
typedef enum {
    SEND_STATE_IDLE = 0,
    SEND_STATE_PREPARING,   // waiting for prepare-upload response
    SEND_STATE_UPLOADING,   // actively sending a file
    SEND_STATE_DONE,        // all files sent
    SEND_STATE_ERROR
} SendState;

#define MAX_SEND_FILES 10000

typedef struct {
    SendState state;
    int       current_file;      // index into send queue
    int       total_files;
    size_t    bytes_sent;
    size_t    bytes_total;
    size_t    bytes_sent_all_files;
    size_t    bytes_total_all_files;
    float     speed_kbps;
    char      status_msg[256];
    char      current_name[256];
    int       cancel_requested;

    // internals
    int       peer_sock;         // TCP socket to receiver
    char      session_id[128];
    char      (*file_tokens)[128]; // token per file from prepare-upload
    SceUID    send_thread_id;
} ActiveSend;

extern ActiveSend active_send;
extern Peer       peer_list[MAX_PEERS];
extern int        peer_count;

// ─── API ──────────────────────────────────────────────────────────────────────
// Register a discovered peer (called by discovery listener)
void sender_add_peer(const char *alias, const char *ip, int port, const char *fingerprint);

// Start sending a list of files to the given peer index
// files[] is an array of full vita paths, names[] their display names, sizes[] their sizes
int sender_start(int peer_index,
                 const char *paths[], const char *names[], const size_t *sizes,
                 int file_count);

void sender_poll(void);   // call each frame; advances state machine
void sender_cancel(void);

#endif // SENDER_H
