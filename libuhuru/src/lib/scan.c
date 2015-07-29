#include "libuhuru-config.h"
#include <libuhuru/module.h>
#include <libuhuru/scan.h>
#include "alert.h"
#include "conf.h"
#include "dir.h"
#include "modulep.h"
#include "protocol.h"
#include "quarantine.h"
#include "statusp.h"
#include "uhurup.h"
#include "unixsock.h"

#include <assert.h>
#include <glib.h>
#include <magic.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>

struct callback_entry {
  uhuru_scan_callback_t callback;
  void *callback_data;
};

struct local_scan {
  GThreadPool *thread_pool;  
  GPrivate *private_magic_key;
};

struct remote_scan {
  char *sock_path;
  int sock;
  struct protocol_handler *handler;
};

struct uhuru_scan {
  struct uhuru *uhuru;
  const char *path;
  enum uhuru_scan_flags flags;
  GArray *callbacks;
  int is_remote;

  union {
    struct local_scan local;
    struct remote_scan remote;
  };
};

static void uhuru_scan_call_callbacks(struct uhuru_scan *scan, struct uhuru_report *report);

/* 
 * **************************************************
 * Local version
 * *************************************************
 */

static void local_scan_init(struct uhuru_scan *scan)
{
  scan->local.thread_pool = NULL;
  scan->local.private_magic_key = NULL;

  uhuru_scan_add_callback(scan, alert_callback, NULL);
  uhuru_scan_add_callback(scan, quarantine_callback, NULL);
}

static enum uhuru_file_status local_scan_apply_modules(const char *path, const char *mime_type, GPtrArray *mod_array,  struct uhuru_report *report)
{
  enum uhuru_file_status current_status = UHURU_UNDECIDED;
  int i;

  for (i = 0; i < mod_array->len; i++) {
    struct uhuru_module *mod = (struct uhuru_module *)g_ptr_array_index(mod_array, i);
    enum uhuru_file_status mod_status;
    char *mod_report = NULL;

    if (mod->mod_status != UHURU_MOD_OK)
      continue;

#if 0
    if (uhuru_get_verbose(u) >= 2)
      printf("UHURU: module %s: scanning %s\n", mod->name, path);
#endif

    mod_status = (*mod->scan)(path, mime_type, mod->data, &mod_report);

#if 0
    printf("UHURU: module %s: scanning %s -> %s\n", mod->name, path, uhuru_status_str(mod_status));
#endif

    if (uhuru_file_status_cmp(current_status, mod_status) < 0) {
      current_status = mod_status;
      uhuru_report_change(report, mod_status, (char *)mod->name, mod_report);
    } else if (mod_report != NULL)
      free(mod_report);

#if 0
    printf("UHURU: current status %s\n", uhuru_file_status_str(current_status));
#endif

    if (current_status == UHURU_WHITE_LISTED || current_status == UHURU_MALWARE)
      break;
  }

  return current_status;
}

static void local_scan_file(struct uhuru_scan *scan, magic_t magic, const char *path)
{
  enum uhuru_file_status status;
  struct uhuru_report report;
  GPtrArray *modules;
  char *mime_type;

  uhuru_report_init(&report, path);

  modules = uhuru_get_applicable_modules(scan->uhuru, magic, path, &mime_type);

  if (modules == NULL)
    report.status = UHURU_UNKNOWN_FILE_TYPE;
  else
    status = local_scan_apply_modules(path, mime_type, modules, &report);

  if (uhuru_get_verbose(scan->uhuru) >= 3)
    printf("%s: %s\n", path, uhuru_file_status_str(status));

  uhuru_scan_call_callbacks(scan, &report);

  uhuru_report_destroy(&report);

  free(mime_type);
}

/* Unfortunately, libmagic is not thread-safe. */
/* We create a new magic_t for each thread, and keep it  */
/* in thread's private data, so that it is created only once. */
static void magic_destroy_notify(gpointer data)
{
  magic_close((magic_t)data);
}

static magic_t get_private_magic(struct uhuru_scan *scan)
{
  magic_t m = (magic_t)g_private_get(scan->local.private_magic_key);

  if (m == NULL) {
    m = magic_open(MAGIC_MIME_TYPE);
    magic_load(m, NULL);

    g_private_set(scan->local.private_magic_key, (gpointer)m);
  }

  return m;
}

static void local_scan_entry_thread_fun(gpointer data, gpointer user_data)
{
  struct uhuru_scan *scan = (struct uhuru_scan *)user_data;
  char *path = (char *)data;

  local_scan_file(scan, get_private_magic(scan), path);

  free(path);
}

static void local_scan_entry(const char *full_path, enum dir_entry_flag flags, int errno, void *data)
{
  struct uhuru_scan *scan = (struct uhuru_scan *)data;

  if (flags & DIR_ENTRY_IS_ERROR) {
    struct uhuru_report report;

    uhuru_report_init(&report, full_path);

    report.status = UHURU_IERROR;
    report.mod_report = strdup(strerror(errno));
    uhuru_scan_call_callbacks(scan, &report);

    uhuru_report_destroy(&report);
  }

  if (!(flags & DIR_ENTRY_IS_REG))
    return;

  if (scan->flags & UHURU_SCAN_THREADED)
    g_thread_pool_push(scan->local.thread_pool, (gpointer)strdup(full_path), NULL);
  else
    local_scan_file(scan, NULL, full_path);
}

static int get_max_threads(void)
{
  return 8;
}

static enum uhuru_scan_status local_scan_start(struct uhuru_scan *scan)
{
  if (scan->flags & UHURU_SCAN_THREADED) {
    scan->local.thread_pool = g_thread_pool_new(local_scan_entry_thread_fun, scan, get_max_threads(), FALSE, NULL);
    scan->local.private_magic_key = g_private_new(magic_destroy_notify);
  }

  return UHURU_SCAN_OK;
}

static enum uhuru_scan_status local_scan_run(struct uhuru_scan *scan)
{
  struct stat sb;

  if (stat(scan->path, &sb) == -1) {
    perror("stat");
    /* exit(EXIT_FAILURE); */
  }

  if (S_ISREG(sb.st_mode)) {
    if (scan->flags & UHURU_SCAN_THREADED)
      g_thread_pool_push(scan->local.thread_pool, (gpointer)strdup(scan->path), NULL);
    else
      local_scan_file(scan, NULL, scan->path);
  } else if (S_ISDIR(sb.st_mode)) {
    int recurse = scan->flags & UHURU_SCAN_RECURSE;

    dir_map(scan->path, recurse, local_scan_entry, scan);
  }

  if (scan->flags & UHURU_SCAN_THREADED)
    g_thread_pool_free(scan->local.thread_pool, FALSE, TRUE);

  return UHURU_SCAN_COMPLETED;
}

/* 
 * **************************************************
 * Remote version
 * *************************************************
 */

static void remote_scan_init(struct uhuru_scan *scan)
{
  char *sock_dir;
  GString *sock_path;

  sock_dir = conf_get(scan->uhuru, "remote", "socket-dir");
  assert(sock_dir != NULL);

  sock_path = g_string_new(sock_dir);

  g_string_append_printf(sock_path, "/uhuru-%s", getenv("USER"));
  scan->remote.sock_path = sock_path->str;
  g_string_free(sock_path, FALSE);
}

static void remote_scan_cb_scan_file(struct protocol_handler *h, void *data)
{
  struct uhuru_scan *scan = (struct uhuru_scan *)data;
  char *path = protocol_handler_get_header(h, "Path");
  char *status = protocol_handler_get_header(h, "Status");
  char *mod_name = protocol_handler_get_header(h, "Module-Name");
  char *x_status = protocol_handler_get_header(h, "X-Status");
  char *action = protocol_handler_get_header(h, "Action");
  struct uhuru_report report;

  uhuru_report_init(&report, path);

  report.status = (enum uhuru_file_status)atoi(status);
  report.action = (enum uhuru_action)atoi(action);
  report.mod_name = mod_name;
  if (x_status != NULL)
    report.mod_report = strdup(x_status);

  uhuru_scan_call_callbacks(scan, &report);

  uhuru_report_destroy(&report);
}

static void remote_scan_cb_scan_end(struct protocol_handler *h, void *data)
{
}

static enum uhuru_file_status remote_scan_start(struct uhuru_scan *scan)
{
  scan->remote.sock = client_socket_create(scan->remote.sock_path, 10);
  if (scan->remote.sock < 0)
    return UHURU_IERROR;
  scan->remote.handler = protocol_handler_new(scan->remote.sock, scan->remote.sock);

  protocol_handler_add_callback(scan->remote.handler, "SCAN_FILE", remote_scan_cb_scan_file, scan);
  protocol_handler_add_callback(scan->remote.handler, "SCAN_END", remote_scan_cb_scan_end, scan);

  protocol_handler_send_msg(scan->remote.handler, "SCAN",
			    "Path", scan->path,
			    NULL);
}

static enum uhuru_scan_status remote_scan_run(struct uhuru_scan *scan)
{
  if (protocol_handler_receive(scan->remote.handler) < 0)
    return UHURU_SCAN_COMPLETED;

  return UHURU_SCAN_CONTINUE;
}

/* 
 * **************************************************
 * common API
 * *************************************************
 */

struct uhuru_scan *uhuru_scan_new(struct uhuru *uhuru, const char *path, enum uhuru_scan_flags flags)
{
  struct uhuru_scan *scan = (struct uhuru_scan *)malloc(sizeof(struct uhuru_scan));

  scan->uhuru = uhuru;
  scan->path = (const char *)realpath(path, NULL);
  if (scan->path == NULL) {
    perror("realpath");
    free(scan);
    return NULL;
  }

  scan->flags = flags;
  scan->callbacks = g_array_new(FALSE, FALSE, sizeof(struct callback_entry));
  scan->is_remote = uhuru_is_remote(uhuru);

  if (scan->is_remote)
    remote_scan_init(scan);
  else
    local_scan_init(scan);

  return scan;
}

void uhuru_scan_add_callback(struct uhuru_scan *scan, uhuru_scan_callback_t callback, void *callback_data)
{
  struct callback_entry entry;

  entry.callback = callback;
  entry.callback_data = callback_data;

  g_array_append_val(scan->callbacks, entry);
}

static void uhuru_scan_call_callbacks(struct uhuru_scan *scan, struct uhuru_report *report)
{
  int i;

  for(i = 0; i < scan->callbacks->len; i++) {
    struct callback_entry *entry = &g_array_index(scan->callbacks, struct callback_entry, i);
    uhuru_scan_callback_t callback = entry->callback;

    (*callback)(report, entry->callback_data);
  }
}

int uhuru_scan_get_poll_fd(struct uhuru_scan *scan)
{
  if (scan->is_remote)
    return scan->remote.sock;

  fprintf(stderr, "cannot call uhuru_scan_get_poll_fd() for a local scan\n");

  return -1;
}

enum uhuru_scan_status uhuru_scan_start(struct uhuru_scan *scan)
{
  return (scan->is_remote) ? remote_scan_start(scan) : local_scan_start(scan);
}

enum uhuru_scan_status uhuru_scan_run(struct uhuru_scan *scan)
{
  return (scan->is_remote) ? remote_scan_run(scan) : local_scan_run(scan);
}

void uhuru_scan_free(struct uhuru_scan *scan)
{
  free((char *)scan->path);

  g_array_free(scan->callbacks, TRUE);

  free(scan);
}
