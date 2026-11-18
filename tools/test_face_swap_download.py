import hashlib
import io
import json
import os
from pathlib import Path
import socketserver
import http.server
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock
import zipfile

from download_face_swap_models import download_model


class DownloadTests(unittest.TestCase):
    def info(self, data):
        return {"url": "https://huggingface.co/facefusion/test", "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}

    def response(self, data):
        response = io.BytesIO(data)
        response.headers = {"Content-Length": str(len(data))}
        return response

    def test_verified_atomic_install_and_existing_file_skips_network(self):
        data = b"verified model bytes"
        with tempfile.TemporaryDirectory() as directory:
            events = []
            opener = Mock(return_value=self.response(data))
            target = download_model("model", self.info(data), directory, lambda *event: events.append(event), opener=opener)
            self.assertEqual(target.read_bytes(), data)
            self.assertEqual(events[-1][:3], ("complete", len(data), len(data)))
            opener.reset_mock()
            download_model("model", self.info(data), directory, opener=opener)
            opener.assert_not_called()
            self.assertFalse(list(Path(directory).glob("*.part")))

    def test_corrupt_or_truncated_transfer_keeps_existing_model_and_cleans_partial(self):
        for data in (b"bad!", b"x"):
            with self.subTest(data=data), tempfile.TemporaryDirectory() as directory:
                target = Path(directory) / "model.onnx"
                target.write_bytes(b"previous model")
                with self.assertRaises(RuntimeError):
                    download_model("model", self.info(b"good"), directory, opener=Mock(return_value=self.response(data)))
                self.assertEqual(target.read_bytes(), b"previous model")
                self.assertFalse(list(Path(directory).glob("*.part")))

    def test_interrupted_stream_cleans_partial(self):
        with tempfile.TemporaryDirectory() as directory:
            response = self.response(b"abc")
            response.read = Mock(side_effect=[b"a", ConnectionError("connection lost")])
            with self.assertRaises(ConnectionError):
                download_model("model", self.info(b"abc"), directory, opener=Mock(return_value=response))
            self.assertFalse(list(Path(directory).iterdir()))

    def test_archive_installs_only_the_verified_detector(self):
        detector = b"SCRFD model fixture"
        archive_bytes = io.BytesIO()
        with zipfile.ZipFile(archive_bytes, "w") as archive:
            archive.writestr("det_10g.onnx", detector)
            archive.writestr("../unrelated.onnx", b"ignored")
        data = archive_bytes.getvalue()
        info = self.info(data)
        info.update(archive_sha256=info["sha256"], archive_member="det_10g.onnx",
                    member_size=len(detector), sha256=hashlib.sha256(detector).hexdigest())
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "det_10g.onnx"
            target.write_bytes(b"existing detector")
            with self.assertRaisesRegex(RuntimeError, "Model integrity check failed"):
                download_model("det_10g", dict(info, sha256="0" * 64), directory,
                               opener=Mock(return_value=self.response(data)))
            self.assertEqual(target.read_bytes(), b"existing detector")
            download_model("det_10g", info, directory, opener=Mock(return_value=self.response(data)))
            self.assertEqual(target.read_bytes(), detector)
            self.assertEqual(list(Path(directory).iterdir()), [target])
            opener = Mock()
            download_model("det_10g", info, directory, opener=opener)
            opener.assert_not_called()

    def test_partial_path_must_not_target_a_user_file(self):
        with tempfile.TemporaryDirectory() as directory:
            photo = Path(directory) / "source.jpg"
            photo.write_bytes(b"photo")
            with self.assertRaises(ValueError):
                download_model("model", self.info(b"abc"), directory, partial=photo)
            self.assertEqual(photo.read_bytes(), b"photo")

    @unittest.skipUnless(os.name == "nt" and os.environ.get("FACE_SWAP_TEST_EXE"), "Windows native integration required")
    def test_native_download_progress_cancel_and_model_preservation(self):
        data = b"model fixture" * (1024 * 640)

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                try:
                    for offset in range(0, len(data), 65536):
                        self.wfile.write(data[offset:offset + 65536])
                        self.wfile.flush()
                        time.sleep(.01)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass

        with tempfile.TemporaryDirectory(prefix="face download é ") as directory, http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
            root = Path(directory)
            engine = root / "engine"
            engine.mkdir()
            (engine / "face_swap_worker.py").touch()
            (engine / "download_face_swap_models.py").write_bytes(Path(__file__).with_name("download_face_swap_models.py").read_bytes())
            info = self.info(data)
            info["url"] = f"http://127.0.0.1:{server.server_port}/model"
            (engine / "face_swap_models_manifest.json").write_text(json.dumps({"hyperswap_1b_256": info}))
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                result = subprocess.run([os.environ["FACE_SWAP_TEST_EXE"], "--download-test", sys.executable, str(root)], capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            finally:
                server.shutdown()
                thread.join(3)


if __name__ == "__main__":
    unittest.main()
