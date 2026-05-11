#!/usr/bin/env python3

from pathlib import Path
from SCons.Script import ARGUMENTS, Default, Glob, SConscript

PROJECT_NAME = "gotool_center"
TARGET_DIR = Path("addons") / "gotool_center" / "bin"

platform = ARGUMENTS.get("platform", "")
target = ARGUMENTS.get("target", "")
arch = ARGUMENTS.get("arch", "")

if not platform:
    raise RuntimeError("Missing required SCons argument: platform=windows|linux|macos")

if not target:
    raise RuntimeError("Missing required SCons argument: target=template_debug|template_release")

if not arch:
    raise RuntimeError("Missing required SCons argument: arch=x86_64|arm64|universal")

env = SConscript("godot-cpp/SConstruct")

TARGET_DIR.mkdir(parents=True, exist_ok=True)

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

sources = []
sources += Glob("src/*.cpp")
sources += Glob("src/**/*.cpp")

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
    ])

sqlite_objects = sqlite_env.Object("third-party/sqlite3/sqlite3.c")

library_path = TARGET_DIR / f"lib{PROJECT_NAME}{env['suffix']}{env['SHLIBSUFFIX']}"

library = env.SharedLibrary(
    target=str(library_path),
    source=sources + sqlite_objects,
)

Default(library)