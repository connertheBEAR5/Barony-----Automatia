#!/usr/bin/env python3
"""Stage and validate a traceable Windows x64 Barony Automatia release.

This intentionally accepts every machine-specific location on the command
line.  It copies runtime DLLs only from the selected vcpkg triplet, the
selected Steamworks SDK redistributable, the selected OpenAL root, or the
explicit locally patched PhysicsFS build.  It never searches System32, PATH,
or another game's installation for DLLs.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import shutil
import struct
import sys
import zipfile
from pathlib import Path
from typing import Any, Iterable


class PackageError(RuntimeError):
    """A validation failure that must prevent a release from being staged."""


# DLL name -> (component, vcpkg port or special source, SPDX/notice description,
# license directory). Components not loaded by the PE dependency walk are not
# copied. This map deliberately has no generic fallback: new dependencies need
# an explicit provenance and license review before they can ship.
DLL_COMPONENTS = {
    "openal32.dll": ("OpenAL Soft", "openal-soft", "LGPL-2.0-or-later", "OpenAL-Soft"),
    "steam_api64.dll": ("Steamworks", "steamworks", "Steamworks SDK redistributable", "Steamworks"),
    "sdl2.dll": ("SDL2", "sdl2", "zlib", "SDL2"),
    "sdl2_image.dll": ("SDL2_image", "sdl2-image", "zlib", "SDL2_image"),
    "sdl2_net.dll": ("SDL2_net", "sdl2-net", "zlib", "SDL2_net"),
    "sdl2_ttf.dll": ("SDL2_ttf", "sdl2-ttf", "zlib", "SDL2_ttf"),
    "physfs.dll": ("PhysicsFS", "physfs-local", "zlib", "PhysicsFS"),
    "glew32.dll": ("GLEW", "glew", "See exact upstream multi-notice", "GLEW"),
    "freetype.dll": ("FreeType", "freetype", "FreeType License", "FreeType"),
    "libpng16.dll": ("libpng", "libpng", "libpng", "libpng"),
    "z.dll": ("zlib", "zlib", "zlib", "zlib"),
    "bz2.dll": ("bzip2", "bzip2", "bzip2-1.0.6", "bzip2"),
    "brotlidec.dll": ("Brotli", "brotli", "MIT", "Brotli"),
    "brotlicommon.dll": ("Brotli", "brotli", "MIT", "Brotli"),
    "ogg.dll": ("libogg", "libogg", "BSD-3-Clause", "libogg"),
    "vorbis.dll": ("libvorbis", "libvorbis", "BSD-3-Clause", "libvorbis"),
    "vorbisfile.dll": ("libvorbis", "libvorbis", "BSD-3-Clause", "libvorbis"),
    "vorbisenc.dll": ("libvorbis", "libvorbis", "BSD-3-Clause", "libvorbis"),
    "fmt.dll": ("fmt", "fmt", "MIT", "fmt"),
}

# PE import names are compared case-insensitively on Windows, but the staged
# folder should retain the canonical names supplied by each upstream runtime.
CANONICAL_DLL_NAMES = {
    "openal32.dll": "OpenAL32.dll",
    "steam_api64.dll": "steam_api64.dll",
    "sdl2.dll": "SDL2.dll",
    "sdl2_image.dll": "SDL2_image.dll",
    "sdl2_net.dll": "SDL2_net.dll",
    "sdl2_ttf.dll": "SDL2_ttf.dll",
    "physfs.dll": "physfs.dll",
    "glew32.dll": "glew32.dll",
    "freetype.dll": "freetype.dll",
    "libpng16.dll": "libpng16.dll",
    "z.dll": "z.dll",
    "bz2.dll": "bz2.dll",
    "brotlidec.dll": "brotlidec.dll",
    "brotlicommon.dll": "brotlicommon.dll",
    "ogg.dll": "ogg.dll",
    "vorbis.dll": "vorbis.dll",
    "vorbisfile.dll": "vorbisfile.dll",
    "vorbisenc.dll": "vorbisenc.dll",
    "fmt.dll": "fmt.dll",
}

SYSTEM_DLLS = {
    "advapi32.dll", "avrt.dll", "bcrypt.dll", "cfgmgr32.dll", "combase.dll", "comctl32.dll",
    "comdlg32.dll", "crypt32.dll", "d3d11.dll", "d3d9.dll", "dinput8.dll",
    "dsound.dll", "dwmapi.dll", "gdi32.dll", "glu32.dll", "imm32.dll",
    "iphlpapi.dll", "kernel32.dll", "kernelbase.dll", "mmdevapi.dll", "msimg32.dll",
    "ntdll.dll", "ole32.dll", "oleaut32.dll", "opengl32.dll", "psapi.dll",
    "rpcrt4.dll", "secur32.dll", "setupapi.dll", "shell32.dll", "shcore.dll",
    "shlwapi.dll", "user32.dll", "userenv.dll", "uxtheme.dll", "version.dll",
    "winmm.dll", "winspool.drv", "wintrust.dll", "ws2_32.dll", "wsock32.dll",
    "wtsapi32.dll", "xinput1_4.dll",
}

RUNTIME_DLL_PREFIXES = ("api-ms-win-", "ext-ms-win-")
MSVC_RUNTIME_PREFIXES = ("vcruntime", "msvcp", "ucrtbase", "concrt")
ASSET_DIRECTORIES = (
    "books", "data", "fonts", "images", "items", "lang", "maps", "models",
    "music", "sound", "themes",
)
ASSET_FILES = (
    "gamecontrollerdb.txt", "npcnames-female.txt", "npcnames-male.txt",
    "playernames-female.txt", "playernames-male.txt", "steam_appid.txt",
)


def fail(message: str) -> None:
    raise PackageError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_c_string(data: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(data):
        fail("PE import points outside its file")
    end = data.find(b"\0", offset)
    if end < 0:
        fail("PE import string is not null terminated")
    return data[offset:end].decode("ascii", errors="strict")


def parse_pe(path: Path) -> dict[str, Any]:
    """Return x64 PE metadata and normal/delay-loaded imports without tools."""
    data = path.read_bytes()
    if len(data) < 0x100 or data[:2] != b"MZ":
        fail(f"Not a PE executable: {path}")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        fail(f"Invalid PE header: {path}")
    machine, section_count, _, _, _, optional_size, _ = struct.unpack_from(
        "<HHIIIHH", data, pe_offset + 4)
    optional_offset = pe_offset + 24
    if optional_offset + optional_size > len(data):
        fail(f"Truncated PE optional header: {path}")
    magic = struct.unpack_from("<H", data, optional_offset)[0]
    if magic == 0x20B:
        directory_offset = optional_offset + 112
    elif magic == 0x10B:
        directory_offset = optional_offset + 96
    else:
        fail(f"Unknown PE optional-header format in {path}")
    if directory_offset + 14 * 8 > optional_offset + optional_size:
        fail(f"Truncated PE data directory: {path}")
    size_of_headers = struct.unpack_from("<I", data, optional_offset + 60)[0]
    section_offset = optional_offset + optional_size
    sections: list[tuple[int, int, int, int]] = []
    for index in range(section_count):
        offset = section_offset + index * 40
        if offset + 40 > len(data):
            fail(f"Truncated PE section table: {path}")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, offset + 8)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))

    def rva_to_offset(rva: int) -> int:
        if rva < size_of_headers:
            return rva
        for virtual_address, section_size, raw_offset, raw_size in sections:
            if virtual_address <= rva < virtual_address + section_size:
                result = raw_offset + (rva - virtual_address)
                if result < len(data) and result < raw_offset + raw_size:
                    return result
        fail(f"PE RVA 0x{rva:x} is outside mapped sections in {path.name}")

    def directory(index: int) -> tuple[int, int]:
        return struct.unpack_from("<II", data, directory_offset + index * 8)

    imports: set[str] = set()
    import_rva, import_size = directory(1)
    if import_rva:
        offset = rva_to_offset(import_rva)
        limit = min(len(data), offset + import_size) if import_size else len(data)
        while offset + 20 <= limit:
            original, _, _, name_rva, first_thunk = struct.unpack_from("<IIIII", data, offset)
            if original == 0 and name_rva == 0 and first_thunk == 0:
                break
            imports.add(read_c_string(data, rva_to_offset(name_rva)).lower())
            offset += 20
        else:
            fail(f"Unterminated import directory in {path}")

    delay_rva, delay_size = directory(13)
    if delay_rva:
        offset = rva_to_offset(delay_rva)
        limit = min(len(data), offset + delay_size) if delay_size else len(data)
        while offset + 32 <= limit:
            attributes, name_rva, *remaining = struct.unpack_from("<IIIIIIII", data, offset)
            if attributes == 0 and name_rva == 0 and not any(remaining):
                break
            # Modern MSVC delay-load descriptors set dlattrRva (bit 0). A VA
            # descriptor has no safe file-relative conversion, so fail closed.
            if not attributes & 1:
                fail(f"Unsupported VA-based delay import in {path.name}")
            imports.add(read_c_string(data, rva_to_offset(name_rva)).lower())
            offset += 32
        else:
            fail(f"Unterminated delay-import directory in {path}")
    return {"machine": machine, "imports": sorted(imports)}


def is_system_import(name: str) -> bool:
    return name in SYSTEM_DLLS or name.startswith(RUNTIME_DLL_PREFIXES)


def is_msvc_runtime(name: str) -> bool:
    return name.startswith(MSVC_RUNTIME_PREFIXES) and name.endswith(".dll")


def canonical_dll_name(name: str) -> str:
    return CANONICAL_DLL_NAMES.get(name.lower(), name)


def vcpkg_version(share_dir: Path, port: str) -> str:
    spdx = share_dir / port / "vcpkg.spdx.json"
    if not spdx.is_file():
        fail(f"Missing vcpkg SPDX provenance for {port}: {spdx}")
    try:
        name = json.loads(spdx.read_text(encoding="utf-8"))["name"]
    except (OSError, ValueError, KeyError) as exc:
        fail(f"Cannot read vcpkg SPDX provenance for {port}: {exc}")
    match = re.search(r"@([^\s]+)", name)
    if not match:
        fail(f"Cannot determine vcpkg version from {spdx}")
    return match.group(1)


def copy_file(source: Path, destination: Path) -> None:
    if not source.is_file():
        fail(f"Required input is missing: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def openal_binary_root(root: Path) -> Path:
    candidates = (root / "bin", root)
    for candidate in candidates:
        if (candidate / "OpenAL32.dll").is_file():
            return candidate
    fail(f"OpenAL root does not contain bin/OpenAL32.dll: {root}")


def source_for_dll(name: str, args: argparse.Namespace, vcpkg_bin: Path) -> Path:
    lower = name.lower()
    if lower not in DLL_COMPONENTS:
        fail(f"Unknown third-party DLL dependency: {name}")
    _, port, _, _ = DLL_COMPONENTS[lower]
    if port == "steamworks":
        source = args.steamworks_sdk / "sdk" / "redistributable_bin" / "win64" / "steam_api64.dll"
    elif port == "physfs-local":
        source = args.physfs_dll
    elif port == "openal-soft":
        source = openal_binary_root(args.openal_root) / "OpenAL32.dll"
    else:
        source = vcpkg_bin / name
    if not source.is_file():
        fail(f"Configured source for {name} does not exist: {source}")
    return source


def collect_runtime_closure(executables: Iterable[Path], args: argparse.Namespace,
                            vcpkg_bin: Path, stage: Path) -> dict[str, list[str]]:
    """Copy the complete third-party closure and return the audited PE tree."""
    pending = list(executables)
    inspected: set[str] = set()
    dependencies: dict[str, list[str]] = {}
    while pending:
        current = pending.pop(0)
        key = current.name.lower()
        if key in inspected:
            continue
        inspected.add(key)
        pe = parse_pe(current)
        if pe["machine"] != 0x8664:
            fail(f"x64 validation failed for {current.name}: PE machine 0x{pe['machine']:04x}")
        imports = pe["imports"]
        dependencies[current.name] = imports
        for imported in imports:
            if "fmod" in imported:
                fail(f"FMOD import is forbidden: {current.name} imports {imported}")
            if is_system_import(imported) or is_msvc_runtime(imported):
                continue
            destination = stage / canonical_dll_name(imported)
            if not destination.exists():
                copy_file(source_for_dll(imported, args, vcpkg_bin), destination)
            pending.append(destination)
    return dependencies


def copy_assets(args: argparse.Namespace, stage: Path) -> list[str]:
    copied: list[str] = []
    for directory_name in ASSET_DIRECTORIES:
        source = args.assets_root / directory_name
        if not source.is_dir():
            fail(f"Missing required game asset directory: {source}")
        destination = stage / directory_name
        shutil.copytree(source, destination, dirs_exist_ok=False)
        copied.append(directory_name)
    for filename in ASSET_FILES:
        source = args.assets_root / filename
        if source.is_file():
            copy_file(source, stage / filename)
            copied.append(filename)
    return copied


def archive_modified_physfs(args: argparse.Namespace, destination: Path) -> str:
    source = args.physfs_source
    if not source.is_dir():
        fail(f"Patched PhysicsFS source directory is missing: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for entry in sorted(source.rglob("*")):
            if entry.is_file():
                archive.write(entry, entry.relative_to(source.parent))
    return destination.relative_to(args.output_dir).as_posix()


def copy_component_license(port: str, license_dir: Path, share_dir: Path) -> list[str]:
    source = share_dir / port / "copyright"
    if not source.is_file():
        fail(f"Missing exact vcpkg license material for {port}: {source}")
    destination = license_dir / "copyright"
    copy_file(source, destination)
    return [destination]


def stage_licenses_and_manifest(args: argparse.Namespace, stage: Path,
                                staged_dlls: list[Path], pe_tree: dict[str, list[str]]) -> dict[str, Any]:
    vcpkg_share = args.vcpkg_root / "installed" / args.triplet / "share"
    licenses = stage / "licenses"
    source_dir = stage / "third_party_source"
    licenses.mkdir(exist_ok=True)
    source_dir.mkdir(exist_ok=True)
    license_paths: dict[str, list[str]] = {}
    versions: dict[str, str] = {}
    copied_ports: set[str] = set()

    # Exact license text emitted by the installed OpenAL Soft package includes
    # the LGPL text plus its PFFFT/FFTPACK notice.
    for dll in staged_dlls:
        component, port, _, license_folder = DLL_COMPONENTS[dll.name.lower()]
        if port in copied_ports:
            continue
        copied_ports.add(port)
        if port == "steamworks":
            # Steamworks agreement material is not copied from a private SDK.
            license_paths[port] = []
            versions[port] = args.steamworks_version
        elif port == "physfs-local":
            source_license = args.physfs_source / "LICENSE.txt"
            destination = licenses / license_folder / "LICENSE.txt"
            copy_file(source_license, destination)
            license_paths[port] = [destination.relative_to(stage).as_posix()]
            versions[port] = args.physfs_version
        else:
            destinations = copy_component_license(port, licenses / license_folder, vcpkg_share)
            license_paths[port] = [item.relative_to(stage).as_posix() for item in destinations]
            versions[port] = vcpkg_version(vcpkg_share, port)

    openal_version = versions["openal-soft"]
    if openal_version.split("#", 1)[0] not in args.openal_source_archive.name:
        fail("OpenAL source archive name does not identify the installed OpenAL Soft version")
    openal_archive = source_dir / args.openal_source_archive.name
    copy_file(args.openal_source_archive, openal_archive)
    physfs_archive = source_dir / f"physfs-{args.physfs_version}-automatia-modified-source.zip"
    physfs_archive_rel = archive_modified_physfs(args, physfs_archive)

    dll_entries: list[dict[str, Any]] = []
    for dll in sorted(staged_dlls, key=lambda value: value.name.lower()):
        component, port, license_name, _ = DLL_COMPONENTS[dll.name.lower()]
        if port == "steamworks":
            source_kind = "authorized_steamworks_sdk"
            source_label = "Steamworks SDK/sdk/redistributable_bin/win64/steam_api64.dll"
            source_archive = None
            modified = False
        elif port == "physfs-local":
            source_kind = "locally_patched_source_build"
            source_label = f"PhysicsFS {args.physfs_version} local source build"
            source_archive = physfs_archive_rel
            modified = True
        else:
            source_kind = "vcpkg"
            source_label = f"vcpkg:{port}:{args.triplet}"
            source_archive = (openal_archive.relative_to(stage).as_posix()
                              if port == "openal-soft" else None)
            modified = False
        dll_entries.append({
            "filename": dll.name,
            "component": component,
            "version": versions[port],
            "architecture": "x64",
            "source_kind": source_kind,
            "source_path_or_package": source_label,
            "license": license_name,
            "license_files": license_paths[port],
            "source_archive_if_any": source_archive,
            "modified": modified,
            "sha256": sha256(dll),
        })

    manifest = {
        "schema_version": 1,
        "generated_utc": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat(),
        "build": {
            "configuration": "Release",
            "architecture": "x64",
            "compiler": args.compiler,
            "windows_sdk": args.windows_sdk,
            "vcpkg_triplet": args.triplet,
            "steamworks_enabled": True,
            "audio_backend": "OpenAL Soft (dynamic OpenAL32.dll)",
            "fmod_staged": False,
            "msvc_runtime_strategy": "official VC_redist.x64.exe prerequisite",
        },
        "executables": [{"filename": "barony.exe", "sha256": sha256(stage / "barony.exe")}],
        "dlls": dll_entries,
        "pe_dependency_tree": pe_tree,
        "openal_source_archive": {
            "filename": openal_archive.relative_to(stage).as_posix(),
            "sha256": sha256(openal_archive),
            "modified": False,
        },
        "physicsfs": {
            "version": args.physfs_version,
            "modified": True,
            "source_archive": physfs_archive_rel,
            "patch_summary": args.physfs_patch_summary,
        },
        "msvc_runtime_prerequisite": {
            "filename": "prerequisites/VC_redist.x64.exe",
            "sha256": sha256(stage / "prerequisites" / "VC_redist.x64.exe"),
            "source": "Official Visual Studio Redist/MSVC directory supplied at packaging time",
        },
        "assets_copied": copy_assets.__name__,  # Replaced by the caller with the audited list.
    }
    return manifest


def write_notices(stage: Path, manifest: dict[str, Any]) -> None:
    lines = [
        "Barony Automatia Windows x64 - Third Party Notices",
        "",
        "This index does not replace the full license texts in licenses/.",
        "Steamworks redistribution is subject to the distributor's Steamworks agreement.",
        "",
    ]
    for entry in manifest["dlls"]:
        lines.extend((
            f"Name: {entry['component']}",
            f"Version: {entry['version']}",
            f"Runtime file: {entry['filename']}",
            f"Source/provenance: {entry['source_path_or_package']}",
            f"License: {entry['license']}",
            f"Modified: {'yes' if entry['modified'] else 'no'}",
            "Full license location: " + (", ".join(entry["license_files"]) or
                                       "Not included: Steamworks SDK agreement governs this redistributable."),
            "Corresponding source: " + (entry["source_archive_if_any"] or "Not applicable / not public SDK source."),
            "",
        ))
    lines.extend((
        "Microsoft runtime deployment",
        "Runtime file: prerequisites/VC_redist.x64.exe",
        "Source/provenance: Official Visual Studio Redist/MSVC directory supplied at packaging time.",
        "",
        "PhysicsFS modification note",
        manifest["physicsfs"]["patch_summary"],
        f"Modified source: {manifest['physicsfs']['source_archive']}",
        "",
        "Distribution reminder: rights to redistribute dependency DLLs are separate from rights to",
        "redistribute Barony game assets, DLC assets, Steam content, and third-party mods.",
    ))
    (stage / "THIRD_PARTY_NOTICES.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def prepare_output(args: argparse.Namespace) -> None:
    output = args.output_dir.resolve()
    expected_parent = args.build_dir.resolve().parent
    if output.parent != expected_parent or not output.name.startswith("BaronyAutomatia-"):
        fail("Output directory must be a BaronyAutomatia-* sibling of the selected build directory")
    if output.exists():
        if not args.clean:
            fail(f"Output directory already exists; pass --clean only for this release staging directory: {output}")
        shutil.rmtree(output)
    output.mkdir(parents=True)


def stage_package(args: argparse.Namespace) -> None:
    args.build_dir = args.build_dir.resolve()
    args.output_dir = args.output_dir.resolve()
    args.vcpkg_root = args.vcpkg_root.resolve()
    args.steamworks_sdk = args.steamworks_sdk.resolve()
    args.openal_root = args.openal_root.resolve()
    args.physfs_dll = args.physfs_dll.resolve()
    args.physfs_source = args.physfs_source.resolve()
    args.assets_root = args.assets_root.resolve()
    args.openal_source_archive = args.openal_source_archive.resolve()
    args.vc_redist = args.vc_redist.resolve()
    executable = args.build_dir / "barony.exe"
    if not executable.is_file():
        fail(f"Release barony.exe not found: {executable}")
    prepare_output(args)
    stage = args.output_dir
    copy_file(executable, stage / "barony.exe")
    executables = [stage / "barony.exe"]
    if args.include_editor:
        editor = args.build_dir / "editor.exe"
        if not editor.is_file():
            fail(f"--include-editor was requested but editor.exe is missing: {editor}")
        copy_file(editor, stage / "editor.exe")
        executables.append(stage / "editor.exe")
    copied_assets = copy_assets(args, stage)
    copy_file(args.vc_redist, stage / "prerequisites" / "VC_redist.x64.exe")
    vcpkg_bin = args.vcpkg_root / "installed" / args.triplet / "bin"
    if not vcpkg_bin.is_dir():
        fail(f"vcpkg runtime directory is missing: {vcpkg_bin}")
    pe_tree = collect_runtime_closure(executables, args, vcpkg_bin, stage)
    if not (stage / "OpenAL32.dll").is_file():
        fail("OpenAL32.dll was not discovered in the executable dependency closure")
    staged_dlls = sorted(stage.glob("*.dll"), key=lambda item: item.name.lower())
    manifest = stage_licenses_and_manifest(args, stage, staged_dlls, pe_tree)
    manifest["assets_copied"] = copied_assets
    if args.include_editor:
        manifest["executables"].append({"filename": "editor.exe", "sha256": sha256(stage / "editor.exe")})
    write_notices(stage, manifest)
    (stage / "THIRD_PARTY_MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    validate_package(args.output_dir)
    print(f"Windows release staged and validated: {args.output_dir}")
    print("Bundled DLLs: " + ", ".join(dll.name for dll in staged_dlls))


def validate_package(stage: Path) -> None:
    stage = stage.resolve()
    manifest_path = stage / "THIRD_PARTY_MANIFEST.json"
    if not manifest_path.is_file():
        fail(f"Missing manifest: {manifest_path}")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except ValueError as exc:
        fail(f"Invalid manifest JSON: {exc}")
    if manifest.get("schema_version") != 1:
        fail("Unsupported or missing manifest schema version")
    dll_entries = manifest.get("dlls")
    if not isinstance(dll_entries, list):
        fail("Manifest is missing its DLL entries")
    names = [entry.get("filename", "").lower() for entry in dll_entries]
    if len(names) != len(set(names)):
        fail("Manifest contains duplicate DLL filenames")
    package_dlls = [path.name.lower() for path in stage.glob("*.dll")]
    if len(package_dlls) != len(set(package_dlls)):
        fail("Package contains case-insensitive duplicate DLL filenames")
    if set(package_dlls) != set(names):
        fail("Package DLL list does not exactly match the manifest")
    if any("fmod" in name for name in package_dlls):
        fail("FMOD DLL is present in the package")
    if "openal32.dll" not in package_dlls:
        fail("OpenAL32.dll is missing from the package")
    if not (stage / "third_party_source" / Path(manifest["openal_source_archive"]["filename"]).name).is_file():
        fail("OpenAL corresponding source archive is missing")
    if not (stage / "prerequisites" / "VC_redist.x64.exe").is_file():
        fail("Official VC_redist.x64.exe prerequisite is missing")
    physfs_entries = [entry for entry in dll_entries if entry["filename"].lower() == "physfs.dll"]
    if len(physfs_entries) != 1 or not physfs_entries[0].get("modified"):
        fail("Patched PhysicsFS is missing or not marked modified")
    steam_entries = [entry for entry in dll_entries if entry["filename"].lower() == "steam_api64.dll"]
    if len(steam_entries) != 1 or steam_entries[0].get("source_kind") != "authorized_steamworks_sdk":
        fail("Steamworks redistributable provenance is missing")
    for entry in dll_entries:
        path = stage / entry["filename"]
        if not path.is_file() or sha256(path) != entry.get("sha256"):
            fail(f"Checksum validation failed: {entry.get('filename')}")
        if parse_pe(path)["machine"] != 0x8664:
            fail(f"x64 architecture validation failed: {path.name}")
        if entry["source_kind"] != "authorized_steamworks_sdk":
            license_files = entry.get("license_files", [])
            if not license_files:
                fail(f"Missing license material in manifest: {path.name}")
            for relative in license_files:
                if not (stage / relative).is_file():
                    fail(f"Missing license file for {path.name}: {relative}")
    for executable in manifest.get("executables", []):
        path = stage / executable["filename"]
        if not path.is_file() or sha256(path) != executable.get("sha256"):
            fail(f"Executable checksum validation failed: {executable.get('filename')}")
        if parse_pe(path)["machine"] != 0x8664:
            fail(f"x64 architecture validation failed: {path.name}")

    # Recompute the recursive import closure from staged files. Any unfamiliar
    # third-party import is a hard failure; system and MSVC redist imports are
    # explicitly classified instead of silently bundled.
    pending = [stage / item["filename"] for item in manifest["executables"]]
    seen: set[str] = set()
    available = {path.name.lower() for path in stage.glob("*.dll")}
    while pending:
        current = pending.pop(0)
        if current.name.lower() in seen:
            continue
        seen.add(current.name.lower())
        for imported in parse_pe(current)["imports"]:
            if "fmod" in imported:
                fail(f"FMOD import is forbidden: {current.name} imports {imported}")
            if is_system_import(imported) or is_msvc_runtime(imported):
                continue
            if imported not in available:
                fail(f"Unresolved non-system dependency: {current.name} imports {imported}")
            if imported not in DLL_COMPONENTS:
                fail(f"Unlicensed third-party dependency: {imported}")
            pending.append(stage / imported)
    print(f"Package validation passed: {stage}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validate-only", action="store_true", help="Validate an existing output folder")
    parser.add_argument("--build-dir", type=Path, help="Release directory containing barony.exe")
    parser.add_argument("--output-dir", type=Path, required=True, help="New BaronyAutomatia-* staging directory")
    parser.add_argument("--vcpkg-root", type=Path, help="vcpkg root used for this build")
    parser.add_argument("--triplet", default="x64-windows", help="vcpkg triplet (default: x64-windows)")
    parser.add_argument("--steamworks-sdk", type=Path, help="Authorized Steamworks SDK root")
    parser.add_argument("--steamworks-version", default="SDK 1.65", help="Steamworks SDK version for notices")
    parser.add_argument("--openal-root", type=Path, help="OpenAL Soft root or its bin directory")
    parser.add_argument("--openal-source-archive", type=Path, help="Exact OpenAL Soft source archive")
    parser.add_argument("--physfs-dll", type=Path, help="Locally patched PhysicsFS runtime DLL")
    parser.add_argument("--physfs-source", type=Path, help="Locally patched PhysicsFS source directory")
    parser.add_argument("--physfs-version", default="3.2.0", help="Patched PhysicsFS upstream version")
    parser.add_argument("--physfs-patch-summary", default=(
        "Modified PhysicsFS 3.2.0 Windows profile-path handling: when the normal "
        "GetUserProfileDirectoryW probe yields no path, fall back to USERPROFILE."),
        help="Human-readable patch summary recorded in notices")
    parser.add_argument("--assets-root", type=Path, help="Authorized Barony asset source; executables and DLLs are never copied")
    parser.add_argument("--vc-redist", type=Path, help="Official VC_redist.x64.exe from the installed VS Redist/MSVC directory")
    parser.add_argument("--compiler", default="MSVC x64", help="Compiler provenance string")
    parser.add_argument("--windows-sdk", default="Windows SDK", help="Windows SDK provenance string")
    parser.add_argument("--include-editor", action="store_true", help="Stage editor.exe and validate its dependency closure")
    parser.add_argument("--clean", action="store_true", help="Replace only an existing safe release staging directory")
    args = parser.parse_args()
    if args.validate_only:
        return args
    required = ("build_dir", "vcpkg_root", "steamworks_sdk", "openal_root", "openal_source_archive",
                "physfs_dll", "physfs_source", "assets_root", "vc_redist")
    missing = [name for name in required if getattr(args, name) is None]
    if missing:
        parser.error("Missing required staging arguments: " + ", ".join("--" + name.replace("_", "-") for name in missing))
    return args


def main() -> int:
    try:
        args = parse_args()
        if args.validate_only:
            validate_package(args.output_dir)
        else:
            stage_package(args)
        return 0
    except PackageError as exc:
        print(f"PACKAGING FAILED: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
