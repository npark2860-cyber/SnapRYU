"""Extract the hash-pinned original SPIR-V resources; never execute the input DLL."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

SOURCE_SHA256 = "626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb"


def resources(path):
    data = Path(path).read_bytes()
    def u16(pos): return struct.unpack_from("<H", data, pos)[0]
    def u32(pos): return struct.unpack_from("<I", data, pos)[0]
    assert data[:2] == b"MZ", "not a PE file"
    pe = u32(60)
    assert data[pe:pe + 4] == b"PE\0\0", "invalid PE signature"
    opt = pe + 24
    assert u16(opt) == 0x20B, "PE32+ required"
    sections = opt + u16(pe + 20)
    def offset(rva):
        for i in range(u16(pe + 6)):
            s = sections + 40 * i
            size, base, raw_size, raw = struct.unpack_from("<4I", data, s + 8)
            if base <= rva < base + max(size, raw_size):
                assert rva - base < raw_size
                return raw + rva - base
        raise ValueError(f"unmapped RVA {rva:x}")
    root = offset(u32(opt + 128))
    def entries(relative):
        pos = root + relative
        return [struct.unpack_from("<II", data, pos + 16 + i * 8)
                for i in range(u16(pos + 12) + u16(pos + 14))]
    found = {}
    for typ, directory in entries(0):
        if typ != 10:
            continue
        assert directory & 0x80000000
        for ident, language_dir in entries(directory & 0x7fffffff):
            assert language_dir & 0x80000000
            for language, leaf in entries(language_dir & 0x7fffffff):
                assert not leaf & 0x80000000
                rva, size = struct.unpack_from("<II", data, root + leaf)
                start = offset(rva)
                value = data[start:start + size]
                assert len(value) == size
                if value[:4] == b"\x03\x02\x23\x07":
                    assert len(value) >= 20 and len(value) % 4 == 0
                    assert ident not in found, "duplicate shader resource"
                    found[ident] = value
    return data, found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--verify", type=Path)
    args = parser.parse_args()
    raw, shaders = resources(args.source)
    assert hashlib.sha256(raw).hexdigest() == SOURCE_SHA256, "unsupported original DLL hash"
    assert set(shaders) == set(range(303, 401)), "expected exactly 98 original SPIR-V resources"
    combined = b"".join(struct.pack("<II", i, len(shaders[i])) + shaders[i] for i in sorted(shaders))
    digest = hashlib.sha256(combined).hexdigest()
    manifest = {"source_sha256": SOURCE_SHA256, "source_version": "3.2.2.0",
                "resource_count": len(shaders), "payload_bytes": sum(map(len, shaders.values())),
                "ordered_payload_sha256": digest,
                "resources": [{"id": i, "bytes": len(shaders[i]), "sha256": hashlib.sha256(shaders[i]).hexdigest()}
                              for i in sorted(shaders)]}
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        rc = ['#pragma code_page(65001)', '#include <windows.h>', 'LANGUAGE LANG_ENGLISH, SUBLANG_ENGLISH_US']
        for ident, value in sorted(shaders.items()):
            path = args.out / f"{ident}.spv"
            path.write_bytes(value)
            rc.append(f'{ident} RCDATA "{path.resolve().as_posix()}"')
        (args.out / "shaders.rc").write_text("\n".join(rc) + "\n", encoding="utf-8")
        (args.out / "shader_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        (args.out / "embedded_shader_hash.h").write_text(f'#define LSFG_EMBEDDED_SHADER_HASH "{digest}"\n', encoding="ascii")
    if args.verify:
        artifact, included = resources(args.verify)
        assert included == shaders, "embedded payload differs from original shaders"
        assert struct.unpack_from("<H", artifact, struct.unpack_from("<I", artifact, 60)[0] + 4)[0] == 0xAA64
        print(json.dumps({"arm64": True, "original_shader_bytes_equal": True,
                          "resources": len(included), "sha256": hashlib.sha256(artifact).hexdigest()}))
    else:
        print(f"Extracted {len(shaders)} original SPIR-V resources ({manifest['payload_bytes']} bytes)")


if __name__ == "__main__":
    main()
