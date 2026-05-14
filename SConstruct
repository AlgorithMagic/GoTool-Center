#!/usr/bin/env python3

import shlex
from pathlib import Path
from SCons.Script import ARGUMENTS, Default, SConscript

PROJECT_NAME = "gotool_center"

ADDON_DIR = Path("project") / "addons" / "GoToolCenter"
BIN_DIR = ADDON_DIR / "bin"

GODOT_CPP_SCONSTRUCT = Path("godot-cpp") / "SConstruct"
SQLITE_SOURCE = Path("third-party") / "sqlite3" / "sqlite3.c"
EXTENSION_API_FILE = Path("third-party") / "godot" / "extension_api.json"

TRUE_VALUES = {"1", "true", "yes", "on"}

NATIVE_TEST_FILES = [
    "tests/native/test_main.cpp",
    "tests/native/scanner_benchmark.cpp",
    "tests/native/schema_v2_tests.cpp",
    "tests/native/scanner_native_tests.cpp",
]

FUZZ_SCRIPT_PARSER_SOURCES = [
    "tests/fuzz/native_script_parser_fuzz.cpp",
    "src/project_scanner/native_script_parser.cpp",
    "src/project_scanner/native_scan_rules.cpp",
]


def read_bool_argument(name: str, default: str) -> bool:
    return str(ARGUMENTS.pop(name, default)).lower() in TRUE_VALUES


def require_source_files(paths: list[str]) -> None:
    missing_paths = [path for path in paths if not Path(path).is_file()]

    if missing_paths:
        formatted_paths = "\n".join(f"  - {path}" for path in missing_paths)
        raise RuntimeError(f"Missing required source file(s):\n{formatted_paths}")


def remove_existing_cpp_standard_flags(build_env, active_platform: str) -> None:
    existing_cxxflags = [str(flag) for flag in build_env.get("CXXFLAGS", [])]

    if active_platform == "windows":
        build_env["CXXFLAGS"] = [
            flag for flag in existing_cxxflags if not flag.startswith("/std:")
        ]
    else:
        build_env["CXXFLAGS"] = [
            flag for flag in existing_cxxflags if not flag.startswith("-std=")
        ]


def configure_cpp_standard_and_warnings(build_env, active_platform: str) -> None:
    if active_platform == "windows":
        build_env.AppendUnique(CXXFLAGS=[
            "/std:c++20",
            "/permissive-",
            "/EHsc",
        ])
    elif active_platform in {"linux", "macos"}:
        build_env.AppendUnique(CXXFLAGS=[
            "-std=c++20",
            "-fexceptions",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
        ])


def configure_sqlite_c_flags(build_env, active_platform: str) -> None:
    if active_platform == "windows":
        build_env.AppendUnique(CCFLAGS=[
            "/std:c17",
            "/wd4090",
            "/wd4996",
        ])
    elif active_platform == "linux":
        build_env.AppendUnique(CCFLAGS=[
            "-std=c17",
            "-Wno-discarded-qualifiers",
            "-Wno-unused-parameter",
            "-Wno-unused-variable",
        ])
    elif active_platform == "macos":
        build_env.AppendUnique(CCFLAGS=[
            "-std=c17",
            "-Wno-incompatible-pointer-types-discards-qualifiers",
            "-Wno-unused-parameter",
            "-Wno-unused-variable",
        ])


def remove_compiler_flag(build_env, variable_name: str, flag: str) -> None:
    current_flags = build_env.get(variable_name, [])

    if isinstance(current_flags, str):
        values = [current_flags]
    else:
        values = list(current_flags)

    filtered_values = []
    for value in values:
        value_text = str(value)

        if value_text == flag:
            continue

        if flag in shlex.split(value_text):
            remaining = [piece for piece in shlex.split(value_text) if piece != flag]

            if remaining:
                filtered_values.append(" ".join(remaining))
            continue

        filtered_values.append(value)

    build_env[variable_name] = filtered_values


def remove_gnu_unique_for_clang(build_env) -> None:
    for variable_name in ("CFLAGS", "CCFLAGS", "CXXFLAGS"):
        remove_compiler_flag(build_env, variable_name, "-fno-gnu-unique")


def configure_native_diagnostic_flags(
    build_env,
    active_platform: str,
    active_sanitizer: str,
    coverage_enabled: bool,
) -> None:
    if active_sanitizer == "none" and not coverage_enabled:
        return

    if active_platform != "linux":
        raise RuntimeError(
            "coverage=1 and sanitizer=... are currently supported only on Linux. "
            "Use WSL2/Linux for local sanitizer and coverage runs."
        )

    # godot-cpp enables this GCC-specific flag for hot reload in template_debug.
    # Sanitizer builds force clang, which may reject it.
    remove_gnu_unique_for_clang(build_env)

    if active_sanitizer == "asan_ubsan":
        build_env.Replace(CXX="clang++")
        build_env.Replace(CC="clang")

        build_env.AppendUnique(CXXFLAGS=[
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-O1",
            "-g",
        ])

        build_env.AppendUnique(CCFLAGS=[
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-O1",
            "-g",
        ])

        build_env.AppendUnique(LINKFLAGS=[
            "-fsanitize=address,undefined",
        ])

    elif active_sanitizer == "tsan":
        if coverage_enabled:
            raise RuntimeError("coverage=1 should not be combined with sanitizer=tsan")

        build_env.Replace(CXX="clang++")
        build_env.Replace(CC="clang")

        build_env.AppendUnique(CXXFLAGS=[
            "-fsanitize=thread",
            "-fno-omit-frame-pointer",
            "-O1",
            "-g",
        ])

        build_env.AppendUnique(CCFLAGS=[
            "-fsanitize=thread",
            "-fno-omit-frame-pointer",
            "-O1",
            "-g",
        ])

        build_env.AppendUnique(LINKFLAGS=[
            "-fsanitize=thread",
        ])

    elif active_sanitizer != "none":
        raise RuntimeError(
            "Unsupported sanitizer. Use sanitizer=none, sanitizer=asan_ubsan, or sanitizer=tsan."
        )

    if coverage_enabled:
        build_env.AppendUnique(CXXFLAGS=[
            "--coverage",
            "-O0",
            "-g",
        ])

        build_env.AppendUnique(CCFLAGS=[
            "--coverage",
            "-O0",
            "-g",
        ])

        build_env.AppendUnique(LINKFLAGS=[
            "--coverage",
        ])


def shared_library_extension_for_platform(active_platform: str) -> str:
    if active_platform == "windows":
        return ".dll"

    if active_platform == "linux":
        return ".so"

    if active_platform == "macos":
        return ".dylib"

    raise RuntimeError(f"Unsupported platform: {active_platform}")


def collect_extension_cpp_sources(doc_data_source: Path) -> list[str]:
    sources = sorted(
        path.as_posix()
        for path in Path("src").rglob("*.cpp")
        if path.as_posix() != doc_data_source.as_posix()
    )

    if not sources:
        raise RuntimeError("No C++ source files found under src/")

    return sources


def collect_native_testable_production_sources() -> list[str]:
    collected_sources: list[str] = []
    seen_sources: set[str] = set()

    excluded_sources = {
        (Path("src") / "gen" / "doc_data.gen.cpp").as_posix(),
        (Path("src") / "register_types.cpp").as_posix(),
    }

    def append_source(path: Path) -> None:
        source_path = path.as_posix()

        if source_path in excluded_sources:
            return

        if source_path in seen_sources:
            return

        seen_sources.add(source_path)
        collected_sources.append(source_path)

    database_root = Path("src") / "database"
    if database_root.is_dir():
        for path in sorted(database_root.rglob("*.cpp")):
            append_source(path)

    project_scanner_root = Path("src") / "project_scanner"
    if project_scanner_root.is_dir():
        for path in sorted(project_scanner_root.rglob("native_*.cpp")):
            append_source(path)

    if not collected_sources:
        raise RuntimeError(
            "No native-testable production source files found. "
            "Expected files under src/database/ or src/project_scanner/native_*.cpp."
        )

    return collected_sources


def configure_libfuzzer_flags(build_env, active_platform: str) -> None:
    if active_platform != "linux":
        raise RuntimeError("fuzz=1 is currently supported only on Linux/Clang")

    # Keep clang-compatible flags in fuzz-only builds as well.
    remove_gnu_unique_for_clang(build_env)

    build_env.Replace(CXX="clang++")
    build_env.Replace(CC="clang")

    build_env.AppendUnique(CXXFLAGS=[
        "-fsanitize=fuzzer,address,undefined",
        "-fno-omit-frame-pointer",
        "-O1",
        "-g",
    ])

    build_env.AppendUnique(CCFLAGS=[
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-O1",
        "-g",
    ])

    build_env.AppendUnique(LINKFLAGS=[
        "-fsanitize=fuzzer,address,undefined",
    ])


platform = ARGUMENTS.get("platform", "")
build_target = ARGUMENTS.get("target", "")
arch = ARGUMENTS.get("arch", "")

build_extension = read_bool_argument("extension", "1")
build_doctest = read_bool_argument("doctest", "0")
build_compiledb = read_bool_argument("compiledb", "1")
build_coverage = read_bool_argument("coverage", "0")
build_fuzz = read_bool_argument("fuzz", "0")

sanitizer = str(ARGUMENTS.pop("sanitizer", "none")).lower()

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

if sanitizer not in {"none", "asan_ubsan", "tsan"}:
    raise RuntimeError(
        "Unsupported sanitizer. Use sanitizer=none, sanitizer=asan_ubsan, or sanitizer=tsan."
    )

if build_fuzz and sanitizer == "tsan":
    raise RuntimeError("fuzz=1 cannot be combined with sanitizer=tsan")

if build_coverage and sanitizer == "tsan":
    raise RuntimeError("coverage=1 cannot be combined with sanitizer=tsan")

if not build_extension and not build_doctest and not build_fuzz:
    raise RuntimeError(
        "No build target selected. Use extension=1, doctest=1, and/or fuzz=1."
    )

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

remove_existing_cpp_standard_flags(env, platform)
configure_cpp_standard_and_warnings(env, platform)

env.AppendUnique(CPPPATH=[
    "src",
    "third-party/sqlite3",
    "third-party/doctest",
])

env.AppendUnique(CPPDEFINES=[
    "SQLITE_THREADSAFE=1",
    "SQLITE_OMIT_LOAD_EXTENSION",
])

configure_native_diagnostic_flags(
    build_env=env,
    active_platform=platform,
    active_sanitizer=sanitizer,
    coverage_enabled=build_coverage,
)

object_dir = Path("build") / "scons" / platform / build_target / arch
object_dir.mkdir(parents=True, exist_ok=True)

default_targets = []
compilation_database_inputs = []

doc_data_source = Path("src") / "gen" / "doc_data.gen.cpp"

if build_extension:
    cpp_sources = collect_extension_cpp_sources(doc_data_source)

    if build_target == "template_debug":
        doc_class_files = sorted(
            path.as_posix()
            for path in Path("doc_classes").glob("*.xml")
        )

        if doc_class_files:
            doc_gen_dir = doc_data_source.parent
            doc_gen_dir.mkdir(parents=True, exist_ok=True)

            doc_data = env.GodotCPPDocData(
                doc_data_source.as_posix(),
                source=doc_class_files,
            )

            cpp_sources.append(doc_data)

    sqlite_env = env.Clone()
    configure_sqlite_c_flags(sqlite_env, platform)

    sqlite_objects = sqlite_env.SharedObject(
        target=(object_dir / "sqlite3").as_posix(),
        source=SQLITE_SOURCE.as_posix(),
    )

    shared_library_extension = shared_library_extension_for_platform(platform)
    library_filename = f"lib{PROJECT_NAME}{env['suffix']}{shared_library_extension}"
    library_target = (BIN_DIR / library_filename).as_posix()

    library = env.SharedLibrary(
        target=library_target,
        source=cpp_sources + sqlite_objects,
    )

    default_targets.append(library)
    compilation_database_inputs.append(library)

    env.Alias("extension", library)

if build_doctest:
    require_source_files(NATIVE_TEST_FILES)

native_test_sources = (
    NATIVE_TEST_FILES
    + collect_native_testable_production_sources()
)

sqlite_test_env = env.Clone()
configure_sqlite_c_flags(sqlite_test_env, platform)

sqlite_test_object = sqlite_test_env.Object(
    target=(object_dir / "sqlite3_native_tests").as_posix(),
    source=SQLITE_SOURCE.as_posix(),
)

native_test_binary = env.Program(
    target=native_test_target,
    source=native_test_sources + sqlite_test_object,
)

default_targets.append(native_test_binary)
compilation_database_inputs.append(native_test_binary)

env.Alias("doctest", native_test_binary)
env.Alias("tests", native_test_binary)

if build_fuzz:
    require_source_files(FUZZ_SCRIPT_PARSER_SOURCES)

    fuzz_env = env.Clone()
    configure_libfuzzer_flags(fuzz_env, platform)

    fuzz_target = (
        Path("build") / "fuzz" / platform / build_target / arch / "native_script_parser_fuzz"
    ).as_posix()

    fuzz_binary = fuzz_env.Program(
        target=fuzz_target,
        source=FUZZ_SCRIPT_PARSER_SOURCES,
    )

    default_targets.append(fuzz_binary)
    compilation_database_inputs.append(fuzz_binary)

    env.Alias("fuzz", fuzz_binary)

if build_compiledb and compilation_database_inputs:
    env.Tool("compilation_db")

    compile_commands = env.CompilationDatabase(
        target="compile_commands.json",
        source=compilation_database_inputs,
    )

    default_targets.append(compile_commands)
    env.Alias("compiledb", compile_commands)

Default(default_targets)