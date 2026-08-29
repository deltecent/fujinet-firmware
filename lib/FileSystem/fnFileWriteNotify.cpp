#include "fnFileWriteNotify.h"

static local_file_written_cb_t s_local_file_written_cb = nullptr;

void set_local_file_written_cb(local_file_written_cb_t cb)
{
    s_local_file_written_cb = cb;
}

void notify_local_file_written(const char *sd_path)
{
    if (s_local_file_written_cb != nullptr)
        s_local_file_written_cb(sd_path);
}
