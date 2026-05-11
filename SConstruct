#!/usr/bin/env python
import os
import sys

# The native code uses exceptions for SQLite initialization and error propagation.
# Keep command-line overrides intact if the caller sets this explicitly.
if ARGUMENTS.get("disable_exceptions") is None:
    ARGUMENTS["disable_exceptions"] = "false"

# Load the build environment configured by godot-cpp
env = SConscript("godot-cpp/SConstruct")

# Define the base name of your GDExtension library
lib_name = "gotool_center"
output_dir = "project/addons/GoToolCenter/bin"

# Setup include directories for the project and third-party libraries
env.Append(CPPPATH=[
    "src",
    "third-party",
    "third-party/doctest",
    "third-party/nlohmann-json",
    "third-party/sqlite3",
    "third-party/spdlog/include"
])

# Gather C++ source files from the src directory
sources = Glob("src/*.cpp") + Glob("src/*/*.cpp")

# Gather third-party source files
sources += Glob("third-party/sqlite3/*.c")
sources += Glob("third-party/spdlog/src/*.cpp")

# Build the shared library, appending the godot-cpp suffix (e.g. .windows.template_debug.x86_64.dll)
library = env.SharedLibrary(
    target=f"{output_dir}/lib{lib_name}{env['suffix']}{env['SHLIBSUFFIX']}",
    source=sources
)

Default(library)
