"""Compile REDscript in an isolated workspace without Windows error dialogs."""

from argparse import ArgumentParser
from ctypes import CDLL, c_char_p, c_void_p
from pathlib import Path
from shutil import copyfile
from sys import stderr
from tempfile import mkdtemp

ROOT = Path(__file__).resolve().parents[1]


def bind(library, name, result_type, *arguments):
    try:
        function = getattr(library, name)
    except AttributeError as error:
        raise RuntimeError(
            f"The supplied compiler does not export {name}. "
            "Use the project's reference REDscript compiler (see README)."
        ) from error
    function.restype = result_type
    function.argtypes = arguments
    return function


def compile_scripts(library_path, r6, cache, registered):
    compiler = CDLL(str(library_path))
    new = bind(compiler, "scc_settings_new", c_void_p, c_char_p)
    disable_popup = bind(compiler, "scc_settings_disable_error_popup", None, c_void_p)
    set_cache = bind(compiler, "scc_settings_set_custom_cache_file", None, c_void_p, c_char_p)
    set_output = bind(compiler, "scc_settings_set_output_cache_file", None, c_void_p, c_char_p)
    add_path = bind(compiler, "scc_settings_add_script_path", None, c_void_p, c_char_p)
    compile_redscript = bind(compiler, "scc_compile", c_void_p, c_void_p)
    get_success = bind(compiler, "scc_get_success", c_void_p, c_void_p)
    free_result = bind(compiler, "scc_free_result", None, c_void_p)

    settings = new(str(r6).encode("utf-8"))
    if not settings:
        raise RuntimeError("REDscript could not create compilation settings.")
    disable_popup(settings)
    set_cache(settings, str(cache).encode("utf-8"))
    set_output(settings, str(cache.with_name("final.redscripts.modded")).encode("utf-8"))
    for path in registered:
        add_path(settings, str(path).encode("utf-8"))

    # scc_compile takes ownership of the settings. We only free the returned result.
    result = compile_redscript(settings)
    if not result:
        raise RuntimeError("REDscript returned no compilation result.")
    try:
        return bool(get_success(result))
    finally:
        free_result(result)


def required_file(path):
    source = path.resolve(strict=True)
    if not source.is_file():
        raise ValueError(f"Expected a file: {path}")
    return source


def snapshot(args):
    compiler = required_file(args.scc_library)
    base_cache = required_file(args.base_cache)
    sources = args.script or [ROOT / "scripts" / "driving_context.reds"]
    registered = [required_file(source) for source in sources]

    loose = args.loose_scripts
    loose_files = []
    if loose is not None:
        loose = loose.resolve(strict=True)
        if not loose.is_dir():
            raise ValueError(f"Expected a loose scripts directory: {loose}")
        loose_files = sorted(path for path in loose.rglob("*.reds") if path.is_file())

    parent = ROOT / "build" / "qa"
    if parent.resolve() != parent:
        raise ValueError(f"Expected the local QA directory inside the project: {parent}")
    parent.mkdir(parents=True, exist_ok=True)
    workspace = Path(mkdtemp(prefix="redscript-", dir=parent))
    print(f"REDscript workspace: {workspace}", flush=True)

    r6 = workspace / "r6"
    compiler_copy = workspace / "scc_lib.dll"
    cache = r6 / "cache" / "final.redscripts"
    loose_copy = r6 / "scripts"
    for directory in (cache.parent, loose_copy, r6 / "logs"):
        directory.mkdir(parents=True)
    copyfile(compiler, compiler_copy)
    copyfile(base_cache, cache)

    for source in loose_files:
        target = loose_copy / source.relative_to(loose)
        target.parent.mkdir(parents=True, exist_ok=True)
        copyfile(source, target)
    registered_copies = []
    for index, source in enumerate(registered, start=1):
        target = workspace / "registered" / str(index) / source.name
        target.parent.mkdir(parents=True)
        copyfile(source, target)
        registered_copies.append(target)

    return compiler_copy, r6, cache, registered_copies


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("--scc-library", required=True, type=Path, help="Path to scc_lib.dll")
    parser.add_argument("--base-cache", required=True, type=Path, help="Vanilla final.redscripts bundle or backup")
    parser.add_argument("--script", action="append", type=Path,
                        help="Explicit registered script, repeatable. Defaults to the project's driving_context.reds.")
    parser.add_argument("--loose-scripts", type=Path, help="Optional directory containing additional loose .reds files")
    args = parser.parse_args()

    try:
        compiler, r6, cache, registered = snapshot(args)
        succeeded = compile_scripts(compiler, r6, cache, registered)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"REDscript check could not run: {error}", file=stderr)
        return 2

    log = r6 / "logs" / "redscript_rCURRENT.log"
    if not succeeded:
        print(f"REDscript compilation failed. Log: {log}", file=stderr)
        return 1
    print(f"REDscript compilation passed. Log: {log}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())