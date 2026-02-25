#!/usr/bin/env python3
"""PE analysis tool for Soldier of Fortune binaries"""
import struct
import datetime
import sys

def analyze_pe(filepath):
    with open(filepath, 'rb') as f:
        data = f.read()

    if data[:2] != b'MZ':
        print('  NOT a valid PE file!')
        return

    pe_offset = struct.unpack_from('<I', data, 0x3C)[0]
    sig = data[pe_offset:pe_offset+4]
    if sig != b'PE\x00\x00':
        print(f'  Invalid PE signature: {sig}')
        return

    # COFF header
    coff = pe_offset + 4
    machine = struct.unpack_from('<H', data, coff)[0]
    num_sections = struct.unpack_from('<H', data, coff+2)[0]
    timestamp = struct.unpack_from('<I', data, coff+4)[0]
    opt_header_size = struct.unpack_from('<H', data, coff+16)[0]
    characteristics = struct.unpack_from('<H', data, coff+18)[0]

    try:
        ts = datetime.datetime.fromtimestamp(timestamp)
    except:
        ts = 'invalid'

    machines = {0x14c: 'i386', 0x8664: 'x86-64', 0x1c0: 'ARM'}
    print(f'  Machine: {machines.get(machine, hex(machine))}')
    print(f'  Sections: {num_sections}')
    print(f'  Timestamp: {ts} (0x{timestamp:08X})')
    print(f'  Characteristics: 0x{characteristics:04X}')

    # Optional header
    opt = coff + 20
    opt_magic = struct.unpack_from('<H', data, opt)[0]
    pe_type = "PE32" if opt_magic == 0x10B else "PE32+" if opt_magic == 0x20B else "unknown"
    print(f'  Format: {pe_type}')

    linker_major = data[opt+2]
    linker_minor = data[opt+3]
    print(f'  Linker version: {linker_major}.{linker_minor}')

    code_size = struct.unpack_from('<I', data, opt+4)[0]
    entry_point = struct.unpack_from('<I', data, opt+16)[0]
    image_base = struct.unpack_from('<I', data, opt+28)[0]
    section_align = struct.unpack_from('<I', data, opt+32)[0]
    image_size = struct.unpack_from('<I', data, opt+56)[0]
    subsystem = struct.unpack_from('<H', data, opt+68)[0]

    subsystems = {1: 'Native', 2: 'Windows GUI', 3: 'Windows Console'}
    print(f'  Code size: 0x{code_size:X} ({code_size:,} bytes)')
    print(f'  Entry point: 0x{entry_point:08X}')
    print(f'  Image base: 0x{image_base:08X}')
    print(f'  Image size: 0x{image_size:X} ({image_size:,} bytes)')
    print(f'  Subsystem: {subsystems.get(subsystem, subsystem)}')

    # Helper: RVA to file offset
    section_start = opt + opt_header_size
    sections = []
    for i in range(num_sections):
        sec = section_start + i * 40
        name = data[sec:sec+8].rstrip(b'\x00').decode('ascii', errors='replace')
        virt_size = struct.unpack_from('<I', data, sec+8)[0]
        virt_addr = struct.unpack_from('<I', data, sec+12)[0]
        raw_size = struct.unpack_from('<I', data, sec+16)[0]
        raw_ptr = struct.unpack_from('<I', data, sec+20)[0]
        chars = struct.unpack_from('<I', data, sec+36)[0]
        sections.append((name, virt_addr, virt_size, raw_ptr, raw_size, chars))

    def rva_to_offset(rva):
        for name, va, vs, rp, rs, ch in sections:
            if va <= rva < va + max(vs, rs):
                return rva - va + rp
        return None

    # Print sections
    print(f'\n  Sections:')
    for name, va, vs, rp, rs, chars in sections:
        flags = []
        if chars & 0x20: flags.append('CODE')
        if chars & 0x40: flags.append('IDATA')
        if chars & 0x80: flags.append('UDATA')
        if chars & 0x20000000: flags.append('EXEC')
        if chars & 0x40000000: flags.append('READ')
        if chars & 0x80000000: flags.append('WRITE')
        print(f'    {name:8s} VA:0x{va:08X} VSize:0x{vs:08X} Raw:0x{rp:08X} RSize:0x{rs:08X} [{"|".join(flags)}]')

    # Data directories
    num_dirs = struct.unpack_from('<I', data, opt+92)[0]
    dir_names = ['Export', 'Import', 'Resource', 'Exception', 'Security',
                 'BaseReloc', 'Debug', 'Copyright', 'GlobalPtr', 'TLS',
                 'LoadConfig', 'BoundImport', 'IAT', 'DelayImport', 'CLR']

    print(f'\n  Data Directories:')
    for i in range(min(num_dirs, 15)):
        rva = struct.unpack_from('<I', data, opt+96+i*8)[0]
        size = struct.unpack_from('<I', data, opt+96+i*8+4)[0]
        if rva or size:
            dname = dir_names[i] if i < len(dir_names) else f'Dir{i}'
            print(f'    [{dname:12s}] RVA: 0x{rva:08X}  Size: 0x{size:X}')

    # Parse imports
    import_rva = struct.unpack_from('<I', data, opt+96+1*8)[0]
    if import_rva:
        import_off = rva_to_offset(import_rva)
        if import_off is None:
            print('  Could not resolve import directory')
            return

        print(f'\n  Imports:')
        pos = import_off
        while True:
            ilt_rva = struct.unpack_from('<I', data, pos)[0]
            name_rva = struct.unpack_from('<I', data, pos+12)[0]
            if name_rva == 0:
                break

            name_off = rva_to_offset(name_rva)
            if name_off:
                end = data.index(b'\x00', name_off)
                dll_name = data[name_off:end].decode('ascii', errors='replace')
            else:
                dll_name = '???'

            # Count and list imports
            funcs = []
            lookup_rva = ilt_rva if ilt_rva else struct.unpack_from('<I', data, pos+16)[0]
            if lookup_rva:
                loff = rva_to_offset(lookup_rva)
                if loff:
                    p = loff
                    while True:
                        entry = struct.unpack_from('<I', data, p)[0]
                        if entry == 0:
                            break
                        if entry & 0x80000000:
                            funcs.append(f'  ord#{entry & 0xFFFF}')
                        else:
                            hint_off = rva_to_offset(entry)
                            if hint_off:
                                hint = struct.unpack_from('<H', data, hint_off)[0]
                                end2 = data.index(b'\x00', hint_off+2)
                                fname = data[hint_off+2:end2].decode('ascii', errors='replace')
                                funcs.append(fname)
                        p += 4

            print(f'    {dll_name} ({len(funcs)} functions):')
            for fn in funcs:
                print(f'      {fn}')
            pos += 20

    # Parse exports
    export_rva = struct.unpack_from('<I', data, opt+96)[0]
    export_size = struct.unpack_from('<I', data, opt+96+4)[0]
    if export_rva and export_size:
        export_off = rva_to_offset(export_rva)
        if export_off:
            num_funcs = struct.unpack_from('<I', data, export_off+20)[0]
            num_names = struct.unpack_from('<I', data, export_off+24)[0]
            funcs_rva = struct.unpack_from('<I', data, export_off+28)[0]
            names_rva = struct.unpack_from('<I', data, export_off+32)[0]
            ords_rva = struct.unpack_from('<I', data, export_off+36)[0]

            print(f'\n  Exports ({num_names} named, {num_funcs} total):')
            names_off = rva_to_offset(names_rva)
            ords_off = rva_to_offset(ords_rva)
            funcs_off = rva_to_offset(funcs_rva)
            if names_off and ords_off and funcs_off:
                for i in range(num_names):
                    name_rva2 = struct.unpack_from('<I', data, names_off + i*4)[0]
                    ordinal = struct.unpack_from('<H', data, ords_off + i*2)[0]
                    func_rva2 = struct.unpack_from('<I', data, funcs_off + ordinal*4)[0]

                    noff = rva_to_offset(name_rva2)
                    if noff:
                        end3 = data.index(b'\x00', noff)
                        ename = data[noff:end3].decode('ascii', errors='replace')
                    else:
                        ename = '???'
                    print(f'    [{ordinal:3d}] 0x{func_rva2:08X} {ename}')

    return sections, image_base, entry_point

files = [
    (r'D:\recomp\pc\sof\_work\game\SoF.exe', 'SoF.exe (Main Engine)'),
    (r'D:\recomp\pc\sof\_work\game\base\gamex86.dll', 'gamex86.dll (Game Logic)'),
    (r'D:\recomp\pc\sof\_work\game\ref_gl.dll', 'ref_gl.dll (OpenGL Renderer)'),
    (r'D:\recomp\pc\sof\_work\game\base\player.dll', 'player.dll'),
]

for path, name in files:
    print(f'\n{"=" * 70}')
    print(f' {name}')
    print(f'{"=" * 70}')
    analyze_pe(path)
