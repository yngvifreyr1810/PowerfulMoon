"""Compile the actual generator's wrapper around a small original C body."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from gen import fname, wrap_original


class OriginalWrapperTests(unittest.TestCase):
    def test_direct_and_indirect_calls_keep_original_and_allow_interception(self):
        entry = 0x7000123400
        body = [f'void {fname(entry)}(Cpu *c) {{', '    c->x[0] += 3;', '}']
        wrapped = wrap_original(entry, body, 'intercept')
        self.assertTrue(body[0].startswith('void '), 'Wrapping must not modify the caller\'s body')
        source = '#include <stdint.h>\n#include <assert.h>\ntypedef struct {uint64_t x[31];} Cpu;\ntypedef void (*GuestFn)(Cpu *);\n'
        source += '\n'.join(wrapped)
        source += '\nvoid intercept(Cpu *c, GuestFn original) { if (c->x[0] == 7) {c->x[0] = 107; return;} original(c); c->x[0] *= 2;}\n'
        source += f'int main(void) {{ Cpu c = {{0}}; c.x[0]=1; {fname(entry)}(&c); assert(c.x[0]==8); '
        source += f'GuestFn indirect={fname(entry)}; c.x[0]=1; indirect(&c); assert(c.x[0]==8); '
        source += 'c.x[0]=7; indirect(&c); assert(c.x[0]==107); return 0;}\n'
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            code, binary = root/'fixture.c', root/('fixture.exe' if os.name == 'nt' else 'fixture')
            code.write_text(source)
            subprocess.run([os.environ.get('CC', 'clang'), '-O2', str(code), '-o', str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
