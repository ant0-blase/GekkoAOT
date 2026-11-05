"""Real bridge CPU benchmark; state-only synthetic lists, never gameplay FPS.

Run the same script/library settings for both artifacts, in separate processes.
Initialization, shader compilation, presentation and shutdown are not timed.
The exact-byte cache comparison, state application and Python resolver are timed.
"""
import ctypes as c
import json
import os
import struct
import time
from gx_interpolation_probe import main


def verify(gx, rect, frame, pixel, memory):
    if cpu := os.environ.get('GEKKOAOT_BENCH_CPU'):
        os.sched_setaffinity(0, {int(cpu)})
    state = bytearray()
    for i in range(512):
        state += b'\x08' + bytes([0x70 + (i & 7)]) + struct.pack('>I', 9 | 1 << 21 | 4 << 22)
        # TEV state changes update pipeline state without recording standalone
        # viewport/scissor commands. This bounds recorder memory during a CPU
        # benchmark that intentionally submits no draws between list calls.
        state += b'\x61' + struct.pack('>I', 0xc0000000 | 0xfff8 | 1 << 19 | (i & 1) << 20)
    address = 0x60000
    c.memmove(c.addressof(memory) + address, bytes(state), len(state))
    command = c.create_string_buffer(b'\x40' + struct.pack('>II', address, len(state)))
    write = gx.gekkoaot_native_gx_write_burst
    for _ in range(100):
        write(command, 9, 0x80004000)
    iterations = int(os.environ.get('GEKKOAOT_STATE_BENCH_ITERATIONS', '20000'))
    results = []
    for _ in range(5):
        begin_cpu, begin_wall = time.thread_time_ns(), time.monotonic_ns()
        for _ in range(iterations):
            write(command, 9, 0x80004000)
        results.append({'thread_cpu_ns': time.thread_time_ns() - begin_cpu,
                        'wall_ns': time.monotonic_ns() - begin_wall})
    print('GX_STATE_BRIDGE_BENCH ' + json.dumps({
        'kind': 'synthetic state-list bridge submission CPU, not frame time or FPS',
        'cpu_affinity': sorted(os.sched_getaffinity(0)),
        'bytes': len(state), 'operations': 1024, 'iterations': iterations,
        'trials': results}), flush=True)


if __name__ == '__main__':
    main(verify)
