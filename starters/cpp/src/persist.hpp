// Persistence for POST /price: an append-only log on a volume, replayed at
// startup. The graded mix has no POSTs, so durability must be cheap, not fast.
#pragma once

#include <string_view>

namespace obsidio {

// Replay updates recorded at `path` (call after init_data()), then open the
// log for appending. Returns false if the log is unwritable; appends no-op.
bool persist_init(const char* path);

// Record one accepted update. Durable (fdatasync) before returning.
void persist_append(std::string_view symbol, double price);

}  // namespace obsidio
