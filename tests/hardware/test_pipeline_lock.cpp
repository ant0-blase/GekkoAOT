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
  if (!(blocked && proceeded && WIFEXITED(status) && WEXITSTATUS(status) == 0)) return 1;
  close(ready[0]); close(ready[1]); close(acquired[0]); close(acquired[1]);

  // Independent cache domains must not serialize unrelated games. A cancelled
  // process must release its lock without deleting any lock files.
  const auto games = std::filesystem::path(argv[1]) / "games";
  GekkoAOT::Build::PipelineLock game_a(games / "A");
  if (pipe(acquired)) return 2;
  const auto parallel = fork();
  if (parallel < 0) return 2;
  if (parallel == 0) {
    game_a.Release();
    GekkoAOT::Build::PipelineLock game_b(games / "B");
    if (write(acquired[1], "x", 1) != 1) _exit(2);
    for (;;) pause();
  }
  event = {acquired[0], POLLIN, 0};
  const bool independent = poll(&event, 1, 5000) == 1;
  kill(parallel, SIGKILL);
  waitpid(parallel, &status, 0);
  GekkoAOT::Build::PipelineLock recovered(games / "B");
  return independent && WIFSIGNALED(status) ? 0 : 1;
}
