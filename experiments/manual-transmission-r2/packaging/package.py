from argparse import ArgumentParser
from copy import copy
from hashlib import file_digest, sha256
from pathlib import Path
from tempfile import TemporaryDirectory
from zipfile import ZIP_DEFLATED, ZipFile

TARGET_NAME = "Chromaxis-0.4.38-prototype.zip"
INSTALL_PATH = "red4ext/plugins/chrome_axis/"
DLL_ENTRY = INSTALL_PATH + "ChromeAxis.dll"
SCRIPT_ENTRY = INSTALL_PATH + "scripts/driving_context.reds"
GUIDE_FOLDER = INSTALL_PATH + "manual-transmission-probe/"
GUIDE_NAMES = ("README-Prototype.fr.md", "README-Prototype.en.md")


def archive_hash(path):
    with path.open("rb") as stream:
        return file_digest(stream, "sha256").hexdigest().upper()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def verify_archive(candidate, baseline, replacements, guides):
    with ZipFile(candidate) as result:
        expected_paths = baseline.namelist() + list(guides)
        require(result.namelist() == expected_paths, "Unexpected ZIP paths or order")
        require(result.testzip() is None, "R2 failed the ZIP integrity check")

        files = [entry.filename for entry in result.infolist() if not entry.is_dir()]
        require(len(files) == 12, "Unexpected number of packaged files")
        require([path for path in files if path.lower().endswith(".reds")] == [SCRIPT_ENTRY],
                "R2 must contain exactly the standard driving context script")
        require([path for path in files if path.lower().endswith(".dll")] == [DLL_ENTRY],
                "R2 must contain exactly one Chrome Axis DLL")

        for entry in baseline.infolist():
            path = entry.filename
            expected_bytes = replacements[path] if path in replacements else baseline.read(entry)
            require(result.read(path) == expected_bytes, f"Unexpected payload: {path}")

        for path, contents in guides.items():
            require(result.read(path) == contents, f"Guide differs from source: {path}")


def package(standard, dll, target, work, approved_standard_hash, approved_dll_hash):
    require(archive_hash(standard) == approved_standard_hash, "Unexpected standard archive SHA-256")
    require(not target.exists(), "R2 archive already exists")

    dll_contents = dll.read_bytes()
    require(sha256(dll_contents).hexdigest().upper() == approved_dll_hash, "Unexpected Release DLL SHA-256")
    guides = {GUIDE_FOLDER + name: (work / name).read_bytes() for name in GUIDE_NAMES}
    replacements = {DLL_ENTRY: dll_contents}

    with TemporaryDirectory(prefix="package-", dir=target.parent) as stage:
        candidate = Path(stage) / target.name
        with ZipFile(standard) as baseline:
            names = baseline.namelist()
            require(len(names) == len(set(names)), "Duplicate entry in standard archive")
            require(sum(not entry.is_dir() for entry in baseline.infolist()) == 10,
                    "Unexpected standard file count")
            require(DLL_ENTRY in names and SCRIPT_ENTRY in names, "Standard DLL or script missing")
            require(not set(guides).intersection(names), "A guide would replace a standard file")
            require(dll_contents != baseline.read(DLL_ENTRY), "R2 must contain the rebuilt DLL")

            with ZipFile(candidate, "x", compression=ZIP_DEFLATED, compresslevel=9) as result:
                result.comment = baseline.comment
                for entry in baseline.infolist():
                    path = entry.filename
                    contents = replacements[path] if path in replacements else baseline.read(entry)
                    result.writestr(copy(entry), contents, compress_type=entry.compress_type)
                for path, contents in guides.items():
                    result.writestr(path, contents)

            verify_archive(candidate, baseline, replacements, guides)

        require(archive_hash(standard) == approved_standard_hash, "Standard archive changed during packaging")
        candidate_hash = archive_hash(candidate)
        candidate.rename(target)
        require(archive_hash(target) == candidate_hash, "R2 ZIP changed while moving to output")

    print(f"Archive: {target}")
    print(f"Bytes: {target.stat().st_size}")
    print(f"SHA-256: {candidate_hash}")
    print("12 files: 9 original payloads, 1 replacement DLL, 2 guides")
    print(f"Standard SHA-256: {approved_standard_hash}")


if __name__ == "__main__":
    parser = ArgumentParser(description="Package the Chrome Axis 0.4.38 manual transmission R2 prototype.")
    parser.add_argument("--standard", type=Path, required=True, help="Verified stable 0.4.38 ZIP")
    parser.add_argument("--standard-sha256", required=True, help="SHA-256 of the verified stable ZIP")
    parser.add_argument("--dll", type=Path, required=True, help="Built Release ChromeAxis.dll")
    parser.add_argument("--dll-sha256", required=True, help="SHA-256 of the verified Release ChromeAxis.dll")
    parser.add_argument("--output-dir", type=Path, required=True, help="Directory for the resulting R2 ZIP")
    args = parser.parse_args()

    work = Path(__file__).resolve().parent
    args.output_dir.mkdir(parents=True, exist_ok=True)
    package(args.standard, args.dll, args.output_dir / TARGET_NAME, work,
            args.standard_sha256.upper(), args.dll_sha256.upper())
