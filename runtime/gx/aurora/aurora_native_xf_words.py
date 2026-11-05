"""One XF semantic implementation for canonical bytes and native scalar words."""
from pathlib import Path
import re
import sys

root=Path(sys.argv[1])
path=root/'lib/gx/regs.cpp'
text=path.read_text()
updates={}
marker='GEKKOAOT_NATIVE_XF_WORDS'
if marker not in text:
    def replace(before,after):
        global text
        if text.count(before)!=1:raise RuntimeError('Native XF word anchor changed: '+before[:100])
        text=text.replace(before,after)
    replace('bool xf_block_unchanged(u32 firstReg, const u8* data, u32 count) noexcept {',
            'bool xf_block_unchanged(u32 firstReg, const u8* data, u32 count, std::endian endian) noexcept {')
    replace('read_bits<u32>(data + w * 4)', 'read_bits<u32>(data + w * 4, endian)')
    for name in ('viewport','projection'):
        start=text.index('void xf_load_'+name+'(')
        end=text.index('\n}',start)+2
        body=text[start:end]
        changed=body.replace('const u8* data)', 'const u8* data, std::endian endian)')
        changed=re.sub(r'read_bits<(f32|u32)>\(data \+ (\d+)\)',r'read_bits<\1>(data + \2, endian)',changed)
        replace(body,changed)
    replace('void handle_xf(u16 addr, std::span<const u8> data) noexcept {',
            '// '+marker+'\nvoid handle_xf_impl(u16 addr, std::span<const u8> data, std::endian endian) noexcept {')
    replace('copy_xf_data(addr, xfData, count, std::endian::big)', 'copy_xf_data(addr, xfData, count, endian)')
    replace('xf_load_viewport(wordData);','xf_load_viewport(wordData, endian);')
    replace('xf_block_unchanged(0x20, wordData, 7)', 'xf_block_unchanged(0x20, wordData, 7, endian)')
    replace('xf_load_projection(wordData);','xf_load_projection(wordData, endian);')
    replace('const u32 val = read_bits<u32>(wordData);','const u32 val = read_bits<u32>(wordData, endian);')
    replace('} // namespace aurora::gx::fifo', '''void handle_xf(u16 addr, std::span<const u8> data) noexcept {
  handle_xf_impl(addr, data, std::endian::big);
}
void handle_xf_words(u16 addr, std::span<const u32> words) noexcept {
  handle_xf_impl(addr, {reinterpret_cast<const u8*>(words.data()), words.size_bytes()}, std::endian::native);
}
} // namespace aurora::gx::fifo''')
    updates[path]=text
elif 'void handle_xf_words(u16 addr, std::span<const u32> words) noexcept {' not in text:
    raise RuntimeError('Incomplete native XF word overlay')
path=root/'lib/gx/regs.hpp'
text=path.read_text()
if 'void handle_xf_words(' not in text:
    before='void handle_xf(u16 addr, std::span<const u8> data) noexcept;'
    if text.count(before)!=1:raise RuntimeError('Native XF header anchor changed')
    updates[path]=text.replace(before,before+'\nvoid handle_xf_words(u16 addr, std::span<const u32> words) noexcept;')
# Resolve both source/header anchors before mutating either file. A vendor
# revision mismatch must not leave a source-only, uncompilable installation.
for path,text in updates.items():
    path.write_text(text)
