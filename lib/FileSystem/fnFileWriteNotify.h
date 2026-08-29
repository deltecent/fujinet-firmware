#ifndef _FN_FILE_WRITE_NOTIFY_H_
#define _FN_FILE_WRITE_NOTIFY_H_

// Decoupled "a local-SD file was just (re)written" notification.
//
// Writers (the HTTP upload handler, the N: SD network protocol) call
// notify_local_file_written() without depending on the fuji/device layer. The
// device layer registers a handler with set_local_file_written_cb() to react --
// e.g. reopen a disk slot whose mounted image was overwritten so reads see the
// new contents instead of a stale cached handle. This inverts the dependency:
// the fuji layer depends on this notifier, not the writers on the fuji layer.

typedef void (*local_file_written_cb_t)(const char *sd_path);

// Register the handler. Passing nullptr disables notification.
void set_local_file_written_cb(local_file_written_cb_t cb);

// A local-SD file was just (re)written at sd_path (SD-root-relative, e.g.
// "/disks/foo.img"). Invokes the registered handler if any; no-op otherwise.
void notify_local_file_written(const char *sd_path);

#endif // _FN_FILE_WRITE_NOTIFY_H_
