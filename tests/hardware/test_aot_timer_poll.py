"""Independent PPC fixtures, compiled by the local pinned LLVM backend.

No SDK binaries/code required. Skipped when the local toolchain is unavailable.
These prove progress across budget exits, not instruction-exact timer visibility.
"""
import contextlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPILER = ROOT / '.gekkoaot/build/DolRecomp/dolrecomp'
META = ROOT / 'build/bin/gekkoaot-module-meta'
RUNNER = ROOT / 'build/bin/gekkoaot-native-run'
SOURCE = ROOT / '.gekkoaot/src/DolRecomp'


def spr_instruction(spr, write=False):
    return (31 << 26) | (3 << 21) | ((spr & 31) << 16) | ((spr >> 5) << 11) | ((467 if write else 339) << 1)


class AotTimerPoll(unittest.TestCase):
    def check_module(self, module):
        pass

    def check_runtime_output(self, output):
        pass

    def fixtures(self):
        return {
            'pmc': [0x38600040, spr_instruction(952, True), spr_instruction(953),
                    0x2c030064, 0x4081fff8, 0x4e800020],
            'dec': [0x38600064, spr_instruction(22, True), spr_instruction(22),
                    0x2c030000, 0x4080fff8, 0x4e800020],
            'tb': [0x7c6c42e6, 0x2c030064, 0x4081fff8, 0x4e800020],
        }

    @unittest.skipUnless(all(p.exists() for p in (COMPILER, META, RUNNER)), 'local LLVM toolchain required')
    def test_compiled_polling(self):
        artifacts = ROOT / '.gekkoaot/audit-v2/aot-tests'
        artifacts.mkdir(parents=True, exist_ok=True)
        for name, words in self.fixtures().items():
            with self.subTest(name=name), contextlib.nullcontext(tempfile.mkdtemp(prefix=name+'-', dir=artifacts)) as tmp:
                tmp = Path(tmp)
                def run(args, timeout=90, horizon=0):
                    result = subprocess.run([str(a) for a in args], cwd=ROOT, text=True,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            timeout=timeout, env={**os.environ, 'GEKKOAOT_NATIVE_CHAIN_CYCLES': str(horizon)})
                    with (tmp / 'commands.log').open('a') as log:
                        log.write(str(args) + '\n' + result.stdout + '\n')
                    self.assertEqual(result.returncode, 0, result.stdout[-12000:])
                    return result.stdout
                data = bytearray(0x100)
                for offset, value in ((0, 0x100), (0x48, 0x80004000), (0x90, len(words)*4), (0xe0, 0x80004000)):
                    struct.pack_into('>I', data, offset, value)
                data.extend(struct.pack('>' + 'I'*len(words), *words))
                dol = tmp / 'poll.dol'
                dol.write_bytes(data)
                run([COMPILER, '--gamecube', '--cpu', 'gekko', '--backend', 'llvm', '--runtime', 'recompcore',
                     '--targets', 'host', '--game-id', 'GTST01', dol, tmp / 'out'])
                generated = tmp / 'out/generated'
                tables = tmp / 'tables.inc'
                run([META, generated / 'generated.h', generated / 'generated_smc.txt', dol, tables])
                run(['cmake', '-S', ROOT / 'runtime/module', '-B', tmp / 'build', '-G', 'Ninja',
                     '-DGAME_ID=GTST01', f'-DGENERATED_DIR={generated}', f'-DDOLRECOMP_DIR={SOURCE}',
                     f'-DGEKKOAOT_ROOT={ROOT}', f'-DMODULE_TABLES={tables}', '-DCMAKE_BUILD_TYPE=Release'])
                run(['cmake', '--build', tmp / 'build', '-j2'])
                self.check_module(tmp / 'build/gGTST01_recomp.so')
                for horizon in (0, 262144):
                    output = run([RUNNER, '--module', tmp / 'build/gGTST01_recomp.so', '--dol', dol,
                                  '--dispatch-limit', str(getattr(self, 'dispatch_limit', 100000))], timeout=15, horizon=horizon)
                    self.assertIn('status="completed"', output)
                    self.check_runtime_output(output)


if __name__ == '__main__':
    unittest.main()
