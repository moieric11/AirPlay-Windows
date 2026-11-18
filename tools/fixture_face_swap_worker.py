"""Dependency-free process fixture for native face-swap lifecycle tests."""
import argparse
import os
from pathlib import Path
import socket
import time
from face_swap_worker import GREETING, HEADER, MAGIC, read_exact, frame_header

parser = argparse.ArgumentParser()
parser.add_argument("--detector", type=Path, required=True)
parser.add_argument("--detector-kind")
parser.add_argument("--recognizer", required=True)
parser.add_argument("--swapper", required=True)
parser.add_argument("--port", type=int, required=True)
parser.add_argument("--cache-root")
parser.add_argument("--ready-file", type=Path, required=True)
parser.add_argument("--single-client", action="store_true")
parser.add_argument("--restorer")
parser.add_argument("--restore-strength")
parser.add_argument("--gpu-bridge")
parser.add_argument("--mask-shape")
parser.add_argument("--mask-padding",nargs=4)
parser.add_argument("--alignment",choices=("legacy","facefusion"),default="legacy")
parser.add_argument("--face-size",type=int,default=100)
parser.add_argument("--face-scale",type=int,default=100)
parser.add_argument("--animation",action="store_true")
parser.add_argument("--animation-strength",type=int,default=100)
parser.add_argument("--shape-transfer",type=int,default=0)
parser.add_argument("--shape-reset",type=int,default=0)
parser.add_argument("--mask-expansion",type=int,default=0)
parser.add_argument("--mask-blur")
parser.add_argument("--mask-preview")
parser.add_argument("--landmarker")
parser.add_argument("--face-parser")
parser.add_argument("--parser-mode")
parser.add_argument("--occluder")
parser.add_argument("--tensorrt", action="store_true")
parser.add_argument("--swap-model")
parser.add_argument("--swapper-backend")
parser.add_argument("--swapper-builder",type=int,default=5)
args = parser.parse_args()
args.ready_file.with_suffix(".pid").write_text(str(os.getpid()))
args.ready_file.with_suffix(".builder").write_text(str(args.swapper_builder))
if (args.detector.parent / "stall-startup").exists():
    time.sleep(60)
with socket.socket() as server:
    server.bind(("127.0.0.1", args.port))
    server.listen(1)
    temp = args.ready_file.with_name(args.ready_file.name + ".tmp")
    temp.write_text(str(server.getsockname()[1]))
    temp.replace(args.ready_file)
    connection, _ = server.accept()
    with connection:
        _, length = GREETING.unpack(read_exact(connection, 8))
        source = read_exact(connection, length).decode()
        if source == "reject":
            connection.sendall(GREETING.pack(0, 8) + b"Rejected")
        else:
            connection.sendall(GREETING.pack(MAGIC, 4) + b"CUDA")
            if source == "stall":
                time.sleep(60)
            else:
                try:
                    while True:
                        header = read_exact(connection, 16)
                        _, _, size = frame_header(header)
                        payload = read_exact(connection, size)
                        if source == "malformed":
                            connection.sendall(HEADER.pack(MAGIC, 8192, 8192, 0))
                            break
                        connection.sendall(header + payload)
                except (EOFError, ConnectionError):
                    pass
