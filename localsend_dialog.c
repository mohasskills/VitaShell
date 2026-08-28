/*
  LocalSend Dialog for VitaShell
*/

#include "main.h"
#include "init.h"
#include "theme.h"
#include "language.h"
#include "utils.h"
#include "localsend_dialog.h"
#include "localsend_discovery.h"
#include "localsend_sender.h"

#include "message_dialog.h"
#include "uncommon_dialog.h"
#include "io_process.h"
#include "browser.h"

#define LOCALSEND_DIALOG_RESULT_NONE 0
#define LOCALSEND_DIALOG_RESULT_WAITING 1

#define PEERLIST_MAX MAX_PEERS

typedef struct {
  int status;
  int result;

  float scale;
  float x;
  float y;
  float width;
  float height;

  int sel;
} LocalSendDialog;

static LocalSendDialog localsend_dialog;
static FileList localsend_list;

static SceUID sender_thread_id = -1;

int initLocalSendDialog() {
  localsend_dialog.status = LOCALSEND_DIALOG_OPENING;
  localsend_dialog.result = LOCALSEND_DIALOG_RESULT_NONE;

  localsend_dialog.scale = 0.0f;
  localsend_dialog.width = 400.0f;
  localsend_dialog.height = 300.0f;
  localsend_dialog.x = (960.0f - localsend_dialog.width) / 2.0f;
  localsend_dialog.y = (544.0f - localsend_dialog.height) / 2.0f;

  localsend_dialog.sel = 0;

  fileListEmpty(&localsend_list);
  strcpy(localsend_list.path, file_list.path);

  FileListEntry *entry = mark_list.head;
  while (entry != NULL) {
    fileListAddEntry(&localsend_list, fileListCopyEntry(entry), 1);
    entry = entry->next;
  }

  return 0;
}

typedef struct {
    char **paths;
    char **names;
    size_t *sizes;
    int count;
    int capacity;
} LocalSendFileQueue;

static void localsend_queue_add(LocalSendFileQueue *q, const char *abs_path, const char *rel_name, size_t size) {
    if (q->count >= q->capacity) {
        q->capacity = (q->capacity == 0) ? 16 : q->capacity * 2;
        q->paths = realloc(q->paths, q->capacity * sizeof(char*));
        q->names = realloc(q->names, q->capacity * sizeof(char*));
        q->sizes = realloc(q->sizes, q->capacity * sizeof(size_t));
    }
    q->paths[q->count] = strdup(abs_path);
    q->names[q->count] = strdup(rel_name);
    q->sizes[q->count] = size;
    q->count++;
}

static int localsend_queue_add_recursive(LocalSendFileQueue *q, const char *abs_dir, const char *rel_dir) {
    SceUID dfd = sceIoDopen(abs_dir);
    if (dfd < 0) return 0;
    
    int res = 0;
    do {
        SceIoDirent dir;
        memset(&dir, 0, sizeof(SceIoDirent));
        res = sceIoDread(dfd, &dir);
        if (res > 0) {
            char new_abs[1024];
            snprintf(new_abs, sizeof(new_abs), "%s/%s", abs_dir, dir.d_name);
            
            char new_rel[1024];
            if (rel_dir && rel_dir[0] != '\0') {
                snprintf(new_rel, sizeof(new_rel), "%s/%s", rel_dir, dir.d_name);
            } else {
                snprintf(new_rel, sizeof(new_rel), "%s", dir.d_name);
            }

            if (SCE_S_ISDIR(dir.d_stat.st_mode)) {
                localsend_queue_add_recursive(q, new_abs, new_rel);
            } else {
                localsend_queue_add(q, new_abs, new_rel, dir.d_stat.st_size);
            }
        }
    } while (res > 0);
    
    sceIoDclose(dfd);
    return 1;
}

void localsendDialogCtrl() {
  if (pressed_pad[PAD_ENTER]) {
    if (peer_count > 0 && localsend_dialog.sel < peer_count) {
      // Start transfer!
      
      int num_files = localsend_list.length;
      if (num_files == 0) return;
      
      LocalSendFileQueue q = {0};
      
      FileListEntry *entry = localsend_list.head;
      while (entry != NULL) {
          char abs_path[1024];
          snprintf(abs_path, sizeof(abs_path), "%s%s", localsend_list.path, entry->name);
          
          if (entry->is_folder) {
              localsend_queue_add_recursive(&q, abs_path, entry->name);
          } else {
              localsend_queue_add(&q, abs_path, entry->name, entry->size);
          }
          entry = entry->next;
      }
      
      if (q.count > 0) {
          sender_start(localsend_dialog.sel, (const char**)q.paths, (const char**)q.names, q.sizes, q.count);
          
          for (int i = 0; i < q.count; i++) {
              free(q.paths[i]);
              free(q.names[i]);
          }
          free(q.paths);
          free(q.names);
          free(q.sizes);
      }
      
      localsend_dialog.status = LOCALSEND_DIALOG_CLOSING;
    }
  }
  
  if (pressed_pad[PAD_CANCEL]) {
    localsend_dialog.status = LOCALSEND_DIALOG_CLOSING;
  }
  
  if (pressed_pad[PAD_TRIANGLE]) {
    peer_count = 0;
    localsend_dialog.sel = 0;
    discovery_broadcast();
  }

  if (hold_pad[PAD_UP] || hold2_pad[PAD_LEFT_ANALOG_UP]) {
    if (localsend_dialog.sel > 0)
      localsend_dialog.sel--;
  } else if (hold_pad[PAD_DOWN] || hold2_pad[PAD_LEFT_ANALOG_DOWN]) {
    if (localsend_dialog.sel < peer_count - 1)
      localsend_dialog.sel++;
  }
  
  if (localsend_dialog.status == LOCALSEND_DIALOG_CLOSING) {
    if (localsend_dialog.scale > 0.0f) {
      localsend_dialog.scale -= easeOut(0.0f, localsend_dialog.scale, 0.25f, 0.01f);
    } else {
      localsend_dialog.status = LOCALSEND_DIALOG_CLOSED;
      fileListEmpty(&localsend_list);
      extern int added_mark;
      if (added_mark) {
          fileListEmpty(&mark_list);
          added_mark = 0;
      }
    }
  }

  if (localsend_dialog.status == LOCALSEND_DIALOG_OPENING) {
    if (localsend_dialog.scale < 1.0f) {
      localsend_dialog.scale += easeOut(localsend_dialog.scale, 1.0f, 0.25f, 0.01f);
    } else {
      localsend_dialog.status = LOCALSEND_DIALOG_OPENED;
    }
  }
}

void drawLocalSendDialog() {
  if (localsend_dialog.status == LOCALSEND_DIALOG_CLOSED)
    return;

  float r_w = localsend_dialog.width * localsend_dialog.scale;
  float r_h = localsend_dialog.height * localsend_dialog.scale;
  float r_x = localsend_dialog.x + (localsend_dialog.width - r_w) / 2.0f;
  float r_y = localsend_dialog.y + (localsend_dialog.height - r_h) / 2.0f;

  vita2d_draw_rectangle(r_x, r_y, r_w, r_h, DIALOG_BG_COLOR);
  
  if (localsend_dialog.status != LOCALSEND_DIALOG_OPENED)
    return;

  float x = localsend_dialog.x + SHELL_MARGIN_X;
  float y = localsend_dialog.y + SHELL_MARGIN_Y;
  
  vita2d_set_clip_rectangle(localsend_dialog.x, localsend_dialog.y, localsend_dialog.x + localsend_dialog.width, localsend_dialog.y + localsend_dialog.height);
  vita2d_enable_clipping();

  char *title = "Discovered Devices";
  pgf_draw_text(x, y, DIALOG_COLOR, title);

  char *refresh_hint = TRIANGLE " Refresh";
  float hint_y = localsend_dialog.y + localsend_dialog.height - SHELL_MARGIN_Y - 20.0f;
  float hint_x = localsend_dialog.x + ALIGN_CENTER(localsend_dialog.width, pgf_text_width(refresh_hint));
  pgf_draw_text(hint_x, hint_y, DIALOG_COLOR, refresh_hint);

  y += 2.0f * FONT_Y_SPACE;
  
  if (peer_count == 0) {
      pgf_draw_text(x, y, DIALOG_COLOR, "Searching...");
  } else {
      int i;
      for (i = 0; i < peer_count; i++) {
        char info[128];
        snprintf(info, sizeof(info), "%s (%s)", peer_list[i].alias, peer_list[i].ip);
        
        if (localsend_dialog.sel == i) {
          pgf_draw_text(x, y, CONTEXT_MENU_FOCUS_COLOR, info);
        } else {
          pgf_draw_text(x, y, DIALOG_COLOR, info);
        }
        
        y += FONT_Y_SPACE;
      }
  }

  vita2d_disable_clipping();
}

int getLocalSendDialogStatus() {
  return localsend_dialog.status;
}

int getLocalSendDialogResult() {
  return localsend_dialog.result;
}
