// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../tools/pipeline_lock.h"
#include <cassert>
#include <poll.h>
#include <sys/wait.h>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  int ready[2], acquired[2];
  if (pipe(ready) || pipe(acquired)) return 2;
  GekkoAOT::Build::PipelineLock first(argv[1]);
  const auto child = fork();
  if (child < 0) return 2;
  if (child == 0) {
    // Drop the inherited descriptor, then contend as an independent process.
    first.Release();
    const char signal = 'x';
    if (write(ready[1], &signal, 1) != 1) _exit(2);
    GekkoAOT::Build::PipelineLock second(argv[1]);
    if (write(acquired[1], &signal, 1) != 1) _exit(2);
    _exit(0);
  }
  char signal;
  if (read(ready[0], &signal, 1) != 1) return 2;
  pollfd event{acquired[0], POLLIN, 0};
  // The other process must remain blocked while the first owns the lock.
  const bool blocked = poll(&event, 1, 200) == 0;
  first.Release();
  const bool proceeded = poll(&event, 1, 5000) == 1;
  int status = 0;
  waitpid(child, &status, 0);
  return blocked && proceeded && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}
