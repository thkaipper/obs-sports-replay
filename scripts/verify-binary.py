"""Check the shipped PE against an OBS installation. Requires pefile.
Usage: python scripts/verify-binary.py PLUGIN_DLL OBS_BIN_DIRECTORY
"""
import json
import pathlib
import struct
import sys
import pefile

plugin = pathlib.Path(sys.argv[1])
directory = pathlib.Path(sys.argv[2])
pe = pefile.PE(str(plugin))
assert pe.FILE_HEADER.Machine == 0x8664, "plugin is not x64"
expected = {b"avcodec-62.dll", b"avformat-62.dll", b"avutil-60.dll", b"swscale-9.dll"}
assert expected.issubset({d.dll for d in pe.DIRECTORY_ENTRY_IMPORT}), "wrong FFmpeg ABI"
exports = {e.name: e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols}
assert b"obs_module_load" in exports
code = pe.get_data(exports[b"obs_module_ver"], 6)
assert code[0] == 0xB8 and struct.unpack_from("<I", code, 1)[0] == 0x20020002, "wrong OBS API version"
result = {}
for dependency in pe.DIRECTORY_ENTRY_IMPORT:
    name = dependency.dll.decode()
    path = directory / name
    if not path.exists():
        if name.upper().startswith(("API-MS-", "EXT-MS-")) or name.upper() in {"KERNEL32.DLL", "MSVCP140.DLL", "VCRUNTIME140.DLL", "VCRUNTIME140_1.DLL"}:
            result[name] = "system runtime; checked when DLL loads"
            continue
        raise AssertionError(f"Missing dependency: {name}")
    # Qt has more than 8192 exports; the parser's default truncation produces false positives.
    dep = pefile.PE(str(path), max_symbol_exports=100000)
    names = {e.name for e in dep.DIRECTORY_ENTRY_EXPORT.symbols}
    missing = [i.name.decode() for i in dependency.imports if i.name and i.name not in names]
    assert not missing, (name, missing)
    result[name] = "all imported names found"
print(json.dumps({"architecture": "x64", "obs_api": "32.2.2", "dependencies": result}, indent=2))
