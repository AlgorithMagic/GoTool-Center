#!/usr/bin/env python3

from pathlib import Path
from SCons.Script import ARGUMENTS, Default, SConscript

PROJECT_NAME = "gotool_center"
ADDON_DIR = Path("addons") / "GoToolCenter"
BIN_DIR = ADDON_DIR / "bin"

platform = ARGUMENTS.get("platform", "")
target = ARGUMENTS.get("target", "")
arch = ARGUMENTS.get("arch", "")

if not platform:
    raise RuntimeError("Missing required SCons argument: platform=windows|linux|macos")

if not target:
    raise RuntimeError("Missing required SCons argument: target=template_debug|template_release")

if not arch:
    raise RuntimeError("Missing required SCons argument: arch=x86_64|arm64|universal")

godot_cpp_sconstruct = Path("godot-cpp") / "SConstruct"

if not godot_cpp_sconstruct.is_file():
    raise RuntimeError(
        "Missing godot-cpp/SConstruct. "
        "Run: git submodule update --init --recursive"
    )

env = SConscript(str(godot_cpp_sconstruct))

BIN_DIR.mkdir(parents=True, exist_ok=True)

env.Append(CPPPATH=[
    "src",
    "third-party/sqlite3",
])

env.Append(CPPDEFINES=[
    "SQLITE_THREADSAFE=1",
    "SQLITE_OMIT_LOAD_EXTENSION",
])

if platform == "windows":
    env.Append(CXXFLAGS=[
        "/std:c++20",
        "/permissive-",
        "/EHsc",
    ])
else:
    env.Append(CXXFLAGS=[
        "-std=c++20",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
    ])

cpp_sources = [str(path) for path in Path("src").rglob("*.cpp")]

if not cpp_sources:
    raise RuntimeError("No C++ source files found under src/")

sqlite_source = Path("third-party") / "sqlite3" / "sqlite3.c"

if not sqlite_source.is_file():
    raise RuntimeError("Missing third-party/sqlite3/sqlite3.c")

sqlite_env = env.Clone()

if platform == "windows":
    sqlite_env.Append(CCFLAGS=[
        "/wd4090",
        "/wd4996",
    ])
else:
    sqlite_env.Append(CCFLAGS=[
        "-Wno-discarded-qualifiers",
        "-Wno-unused-parameter",
        "-Wno-unused-variable",
    ])

sqlite_objects = sqlite_env.Object(str(sqlite_source))

library_target = str(
    BIN_DIR / f"lib{PROJECT_NAME}{env['suffix']}{env['SHLIBSUFFIX']}"
)

library = env.SharedLibrary(
    target=library_target,
    source=cpp_sources + sqlite_objects,
)

Default(library)