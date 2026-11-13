"""Relocation type census and TLS segment of each module."""
import sys
from collections import Counter
from elftools.elf.elffile import ELFFile
for p in sys.argv[1:]:
    with open(p, 'rb') as f:
        e = ELFFile(f)
        c = Counter()
        for name in ('.rela.dyn', '.rela.plt'):
            s = e.get_section_by_name(name)
            if s:
                for r in s.iter_relocations():
                    c[r['r_info_type']] += 1
        tls = [(hex(s['p_vaddr']), hex(s['p_filesz']), hex(s['p_memsz']), s['p_align']) for s in e.iter_segments() if s['p_type'] == 'PT_TLS']
        init = e.get_section_by_name('.init_array')
        print(p.split('/')[-1], dict(c), 'TLS', tls, 'init_array', init and hex(init['sh_size']), 'preinit', bool(e.get_section_by_name('.preinit_array')))
