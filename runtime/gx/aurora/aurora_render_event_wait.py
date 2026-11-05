#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replace Aurora render idle timeouts with producer/close/stop notifications."""
from pathlib import Path
import sys
root=Path(sys.argv[1]);marker='GEKKOAOT_RENDER_EVENT_WAIT_V1'
p=root/'lib/gfx/render_worker.hpp';s=p.read_text()
if marker not in s:
 anchor='  std::optional<QueueItem> pop_for(std::chrono::milliseconds timeout, bool& closed);'
 assert s.count(anchor)==1
 s=s.replace(anchor,anchor+'\n  std::optional<QueueItem> pop(bool& closed);')
 p.write_text('// '+marker+'\n'+s)
p=root/'lib/gfx/render_worker.cpp';s=p.read_text()
if marker not in s:
 anchor='  g_workerThreadId = std::this_thread::get_id();'
 assert s.count(anchor)==1
 s=s.replace(anchor,anchor+'''
  // push/close already notify the queue predicate. A stop request must do so
  // too, including static jthread destruction; no periodic event pump runs here.
  std::stop_callback stopWake(token, [] { g_queue.close(); });
  const char* eventWaitValue=std::getenv("GEKKOAOT_AURORA_RENDER_EVENT_WAIT");
  const bool eventWait=!(eventWaitValue && eventWaitValue[0]=='0');
''')
 anchor='return g_queue.pop_for(IdlePumpInterval, closed);'
 if anchor not in s:
  anchor='auto item = g_queue.pop_for(IdlePumpInterval, closed);'
  assert s.count(anchor)==1
  s=s.replace(anchor,'auto item = eventWait ? g_queue.pop(closed) : g_queue.pop_for(IdlePumpInterval, closed);')
 else:s=s.replace(anchor,'return eventWait ? g_queue.pop(closed) : g_queue.pop_for(IdlePumpInterval, closed);')
 anchor='std::optional<QueueItem> BoundedQueue::pop_for('
 pos=s.index(anchor)
 s=s[:pos]+'''std::optional<QueueItem> BoundedQueue::pop(bool& closed) {
  std::unique_lock lock{m_mutex};
  m_notEmpty.wait(lock, [&] { return m_closed || !m_items.empty(); });
  closed = m_closed && m_items.empty();
  if (m_items.empty()) return std::nullopt;
  auto item = std::move(m_items.front());
  m_items.pop_front();
  lock.unlock();
  m_notFull.notify_one();
  return item;
}

'''+s[pos:]
 s=s.replace('#include <chrono>','#include <chrono>\n#include <cstdlib>')
 p.write_text('// '+marker+'\n'+s)
