"""Best-effort preparation stages. ORT does not expose TensorRT build percentages."""
from pathlib import Path

_path = None


def configure(ready_file):
    global _path
    _path = Path(ready_file).with_suffix(".progress") if ready_file else None


def report(stage):
    if _path is None:
        return
    try:
        temporary = _path.with_suffix(".progress.tmp")
        temporary.write_text(stage.replace("\n", " ")[:512], encoding="utf-8")
        temporary.replace(_path)
    except OSError:
        # Diagnostics must never prevent inference, even if the GUI is reading.
        pass
