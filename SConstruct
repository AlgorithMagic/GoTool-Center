#!/usr/bin/env python3

from pathlib import Path
from SCons.Script import ARGUMENTS, Default, SConscript

PROJECT_NAME = "gotool_center"

ADDON_DIR = Path("project") / "addons" / "GoToolCenter"
BIN_DIR = ADDON_DIR / "bin"

GODOT_CPP_SCONSTRUCT = Path("godot-cpp") / "SConstruct"
SQLITE_SOURCE = Path("third-party") / "sqlite3" / "sqlite3.c"
EXTENSION_API_FILE = Path("third-party") / "godot" / "extension_api.json"

platform = ARGUMENTS.get("platform", "")
build_target = ARGUMENTS.get("target", "")
arch = ARGUMENTS.get("arch", "")
build_doctest = str(ARGUMENTS.pop("doctest", "0")).lower() in {"1", "true", "yes"}

if not platform:
    raise RuntimeError("Missing required SCons argument: platform=windows|linux|macos")

if not build_target:
    raise RuntimeError("Missing required SCons argument: target=template_debug|template_release")

if not arch:
    raise RuntimeError("Missing required SCons argument: arch=x86_64|arm64|universal")

if platform not in {"windows", "linux", "macos"}:
    raise RuntimeError(f"Unsupported platform: {platform}")

if build_target not in {"template_debug", "template_release"}:
    raise RuntimeError(f"Unsupported target: {build_target}")

if not GODOT_CPP_SCONSTRUCT.is_file():
    raise RuntimeError(
        "Missing godot-cpp/SConstruct. "
        "Run: git submodule update --init --recursive"
    )

if not SQLITE_SOURCE.is_file():
    raise RuntimeError("Missing third-party/sqlite3/sqlite3.c")

if not EXTENSION_API_FILE.is_file():
    raise RuntimeError(
        "Missing third-party/godot/extension_api.json. "
        "Dump it with Godot using --dump-extension-api, then move it into third-party/godot/."
    )

BIN_DIR.mkdir(parents=True, exist_ok=True)

env = SConscript(str(GODOT_CPP_SCONSTRUCT))

existing_cxxflags = [str(flag) for flag in env.get("CXXFLAGS", [])]
if platform == "windows":
    env["CXXFLAGS"] = [flag for flag in existing_cxxflags if not flag.startswith("/std:")]
else:
    env["CXXFLAGS"] = [flag for flag in existing_cxxflags if not flag.startswith("-std=")]

env.AppendUnique(CPPPATH=[
    "src",
    "third-party/sqlite3",
    "third-party/doctest",
])

env.AppendUnique(CPPDEFINES=[
    "SQLITE_THREADSAFE=1",
    "SQLITE_OMIT_LOAD_EXTENSION",
])

if platform == "windows":
    env.AppendUnique(CXXFLAGS=[
        "/std:c++20",
        "/permissive-",
        "/EHsc",
    ])
elif platform in {"linux", "macos"}:
    env.AppendUnique(CXXFLAGS=[
        "-std=c++20",
        "-fexceptions",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
    ])

cpp_sources = sorted(path.as_posix() for path in Path("src").rglob("*.cpp"))

if not cpp_sources:
    raise RuntimeError("No C++ source files found under src/")

object_dir = Path("build") / "scons" / platform / build_target / arch
object_dir.mkdir(parents=True, exist_ok=True)

sqlite_env = env.Clone()

if platform == "windows":
    sqlite_env.AppendUnique(CCFLAGS=[
        "/std:c17",
    ])
    sqlite_env.AppendUnique(CCFLAGS=[
        "/wd4090",
        "/wd4996",
    ])
elif platform == "linux":
    sqlite_env.AppendUnique(CCFLAGS=[
        "-std=c17",
        "-Wno-discarded-qualifiers",
        "-Wno-unused-parameter",
        "-Wno-unused-variable",
    ])
elif platform == "macos":
    sqlite_env.AppendUnique(CCFLAGS=[
        "-std=c17",
        "-Wno-incompatible-pointer-types-discards-qualifiers",
        "-Wno-unused-parameter",
        "-Wno-unused-variable",
    ])

sqlite_objects = sqlite_env.SharedObject(
    target=(object_dir / "sqlite3").as_posix(),
    source=SQLITE_SOURCE.as_posix(),
)

if platform == "windows":
    shared_library_extension = ".dll"
elif platform == "linux":
    shared_library_extension = ".so"
elif platform == "macos":
    shared_library_extension = ".dylib"
else:
    raise RuntimeError(f"Unsupported platform: {platform}")

library_filename = f"lib{PROJECT_NAME}{env['suffix']}{shared_library_extension}"
library_target = (BIN_DIR / library_filename).as_posix()

library = env.SharedLibrary(
    target=library_target,
    source=cpp_sources + sqlite_objects,
)

default_targets = [library]

if build_doctest:
    native_test_sources = [
        "tests/native/test_main.cpp",
        "tests/native/scanner_benchmark.cpp",
        "tests/native/schema_v2_tests.cpp",
        "tests/native/scanner_native_tests.cpp",
        "src/database/gotool_database.cpp",
        "src/database/gotool_schema.cpp",
        "src/database/gotool_project_registry_repository.cpp",
        "src/project_scanner/native_directory_enumerator.cpp",
        "src/project_scanner/native_scan_pipeline.cpp",
        "src/project_scanner/native_scan_rules.cpp",
        "src/project_scanner/native_script_parser.cpp",
    ]

    native_test_env = env

    native_test_target = (
        Path("build") / "tests" / platform / build_target / arch / "gotool_native_tests"
    ).as_posix()

    native_test_binary = native_test_env.Program(
        target=native_test_target,
        source=native_test_sources + [SQLITE_SOURCE.as_posix()],
    )

    default_targets.append(native_test_binary)

Default(default_targets)
