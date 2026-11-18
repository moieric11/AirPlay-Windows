"""Download optional face-swap models from pinned official sources, verified before installation."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def download_model(name, info, directory, report=lambda *args: None, partial=None,
                   verify_only=False, opener=urllib.request.urlopen):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    target = directory / (name + ".onnx")
    report("checking", 0, 0, "Checking existing " + name)
    if target.is_file() and digest(target) == info["sha256"]:
        report("complete", target.stat().st_size, target.stat().st_size, name + ": already verified")
        return target
    if verify_only:
        raise RuntimeError("Missing or mismatched model: " + str(target))
    if partial is None:
        with tempfile.NamedTemporaryFile(dir=directory, prefix=".download-", suffix=".part", delete=False) as output:
            partial = Path(output.name)
        partial.unlink()
    partial = Path(partial)
    if (partial.resolve().parent != directory.resolve() or not partial.name.startswith(".download-")
            or partial.suffix != ".part"):
        raise ValueError("Partial download must use a private .download-*.part file in the model folder")
    received, last_report = 0, 0.0
    total = int(info.get("size", 0))
    request = urllib.request.Request(info["url"], headers={"User-Agent": "AirPlay-Windows-model-setup"})
    created = False
    try:
        report("downloading", 0, total, "Downloading " + name + " from the official publisher")
        with opener(request, timeout=30) as response:
            content_length = int(response.headers.get("Content-Length", 0))
            if total and content_length and total != content_length:
                raise RuntimeError("Unexpected download size; model was not installed")
            total = total or content_length
            sha = hashlib.sha256()
            with partial.open("xb") as output:
                created = True
                while True:
                    block = response.read(1024 * 1024)
                    if not block:
                        break
                    received += len(block)
                    if total and received > total:
                        raise RuntimeError("Download exceeds expected size")
                    output.write(block)
                    sha.update(block)
                    now = time.monotonic()
                    if now - last_report >= .1:
                        report("downloading", received, total, "Downloading " + name)
                        last_report = now
        report("verifying", received, total, "Verifying SHA-256: " + name)
        if (total and received != total) or sha.hexdigest() != info.get("archive_sha256", info["sha256"]):
            raise RuntimeError("Integrity check failed; existing model was preserved: " + name)
        if "archive_member" in info:
            with tempfile.TemporaryFile(dir=directory) as extracted:
                with zipfile.ZipFile(partial) as archive:
                    member = archive.getinfo(info["archive_member"])
                    if member.file_size != info["member_size"]:
                        raise RuntimeError("Unexpected model size in archive: " + name)
                    # Extract just the named file, never paths supplied by the ZIP.
                    with archive.open(member) as source:
                        shutil.copyfileobj(source, extracted)
                extracted.seek(0)
                if hashlib.file_digest(extracted, "sha256").hexdigest() != info["sha256"]:
                    raise RuntimeError("Model integrity check failed: " + name)
                extracted.seek(0)
                with partial.open("wb") as output:
                    shutil.copyfileobj(extracted, output)
        partial.replace(target)
        report("complete", received, received, name + ": installed and SHA-256 verified")
        return target
    except urllib.error.HTTPError as error:
        if error.code in (401, 403):
            raise RuntimeError("Access denied. Open the publisher page and review its access conditions; no login or license gate is bypassed.") from error
        raise
    finally:
        if created:
            partial.unlink(missing_ok=True)


def main():
    manifest = json.loads(Path(__file__).with_name("face_swap_models_manifest.json").read_text())
    parser = argparse.ArgumentParser(description=__doc__)
    engine = Path(__file__).resolve().parent
    default = engine.parent / "swappers" if engine.name == "engine" else engine.parent / "build/Release/face_swap/swappers"
    parser.add_argument("--directory", type=Path, default=default)
    parser.add_argument("--models", nargs="+", choices=tuple(manifest), default=list(manifest))
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--progress-file", type=Path)
    parser.add_argument("--partial-file", type=Path)
    args = parser.parse_args()

    def report(state, received, total, message):
        print(message, flush=True)
        if args.progress_file:
            temporary = args.progress_file.with_suffix(".tmp")
            for attempt in range(5):
                try:
                    temporary.write_text(f"{state}\n{received} {total}\n{message.replace(chr(10), ' ')[:512]}", encoding="utf-8")
                    temporary.replace(args.progress_file)
                    break
                except OSError:
                    # Windows scanners/readers can briefly delay atomic replacement.
                    if attempt<4:time.sleep(.02)
    try:
        for name in args.models:
            download_model(name, manifest[name], args.directory, report, args.partial_file, args.verify_only)
    except Exception as error:
        report("failed", 0, 0, str(error))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
