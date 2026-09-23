"""Independent AOT DOL exercising a real page miss, guest repair and RFI."""
import ctypes
from pathlib import Path
import test_aot_timer_poll as harness


def constant(register, value):
    return [0x3c000000 | (register << 21) | (value >> 16),
            0x60000000 | (register << 21) | (register << 16) | (value & 0xffff)]


def store(address, value):
    return constant(4, address) + constant(3, value) + [0x90640000]


class AotVM(harness.AotTimerPoll):
    dispatch_limit = 100

    def check_module(self, module):
        library = ctypes.CDLL(str(module))
        library.gekkoaot_module_capabilities.restype = ctypes.c_uint32
        self.assertEqual(library.gekkoaot_module_capabilities() & 1, 1)
        # Existing caches made by the old compiler have no capability symbol.
        cached = list((harness.ROOT / '.gekkoaot/cache/modules/GHLE69').glob('*/gGHLE69_recomp.so'))
        if cached:
            old = ctypes.CDLL(str(cached[0]))
            self.assertFalse(hasattr(old, 'gekkoaot_module_capabilities'))

    def check_runtime_output(self, output):
        self.assertIn('GEKKOAOT_VM_MIGRATION_V97=1', output)
        self.assertIn('paging=1', output)
    def fixtures(self):
        # Exception vector independently writes the missing PTE and retries.
        handler = constant(10, 0x17f78) + constant(11, 0xad5e6f38) + [0x916a0000, 0x4c000064]
        words = constant(3, 0x32) + [0x7c600124]  # mtmsr r3: IR/DR enabled
        for index, instruction in enumerate(handler):
            words += store(0x300 + index * 4, instruction)
        words += store(0x17f78, 0) + store(0x17f7c, 0x200002)
        words += store(0x200abc, 0x12345678)
        words += constant(3, 0x5abcde) + [0x7c6701a4]  # mtsr 7,r3
        words += constant(3, 0x10000) + [harness.spr_instruction(25, True)]
        words += constant(4, 0x7e123abc) + [0x80c40000]  # lwz r6,0(r4), faults once
        words += constant(7, 0x12345678) + [0x7c063800, 0x40820014]  # cmpw; bne fail
        words += constant(7, 0xaabbccdd) + [0x90e40000, 0x4e800020, 0x48000000]
        return {'vm_dsi_rfi': words}
