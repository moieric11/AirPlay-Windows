import os
import socket
import subprocess
import threading
import unittest
import sys
import tempfile
import struct
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
from contextlib import redirect_stderr
import io

from face_swap_worker import (GREETING, HEADER, MAGIC, frame_header,
                              GPU_MAGIC, GPU_ERROR, MASK_CONFIG, largest_face, read_exact, require_cuda, require_provider, execution_config, serve_client)


class FakeEngine:
    def set_source(self, path):
        if path == "reject":
            raise ValueError("No face in source")

    def swap(self, data, width, height):
        return data


class ProtocolTests(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('FACE_SWAP_TEST_EXE'), 'Native integration executable required')
    def test_native_live_mask_updates_without_new_frames_or_reconnection(self):
        import time
        events=[];frames=[];errors=[];original_inputs=[]
        class Configuration(dict):
            def __setitem__(self,key,value):
                super().__setitem__(key,value)
                if key=='mask_config':events.append((time.monotonic(),dict(value)))
        config=Configuration(dll=None,cache=None,restorer=None,strength=.6)
        class Engine(FakeEngine):
            def swap(self,data,width,height):
                frames.append((time.monotonic(),dict(config.get('mask_config',{}))))
                original_inputs.append(bytes(data))
                return bytes([config.get('mask_config',{}).get('blur',0)])+data[1:]
        with socket.socket() as listener:
            listener.bind(('127.0.0.1',0));listener.listen(1);listener.settimeout(5)
            def run():
                try:
                    connection,_=listener.accept()
                    with connection:serve_client(connection,Engine(),config)
                except (EOFError,ConnectionError):pass
                except Exception as error:errors.append(error)
            thread=threading.Thread(target=run);thread.start()
            try:
                result=subprocess.run([os.environ['FACE_SWAP_TEST_EXE'],str(listener.getsockname()[1]),'--mask-live-test'],
                    capture_output=True,text=True,timeout=10)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            finally:thread.join(3)
        self.assertFalse(thread.is_alive());self.assertFalse(errors,errors)
        self.assertTrue(events);self.assertTrue(frames)
        expected={'shape':'box','blur':27,'preview':2,'padding':(4,3,2,1),'face_scale':105,'face_size':110,'alignment':0,'expansion':0,'shape_transfer':0,'shape_reset':0,'animation_strength':100}
        self.assertEqual(frames[0][1],expected)
        initial=next(event for event in events if event[1]==expected)
        self.assertLess(initial[0],frames[0][0],'First frame preceded the queued configuration ACK')
        self.assertEqual(len(frames),2,'Preview refresh requires exactly one additional frame')
        self.assertEqual(frames[1][1],dict(expected,blur=31,alignment=1,expansion=4,shape_transfer=60,shape_reset=1,animation_strength=70))
        self.assertEqual(original_inputs[0],original_inputs[1],'Refresh reused a composited output instead of the original image')
        self.assertGreater(frames[1][0]-frames[0][0],.6)
        self.assertEqual(events[-1][1],dict(expected,blur=32,alignment=1,expansion=4,shape_transfer=60,shape_reset=1,animation_strength=70))

    def test_gpu_preparation_finishes_before_any_video_and_reuses_sessions(self):
        from unittest.mock import Mock
        client, worker = socket.socketpair()
        errors = []
        gpu = Mock()
        gpu.process.return_value = 1.0
        gpu.last_timings = {}
        gpu.last_face_count = 1
        gpu.metadata_bytes = 0
        factory = Mock(return_value=gpu)
        config = {"dll": Path("bridge.dll"), "cache": Path("swappers/cuda-kernels"),
                  "restorer": None, "strength": .6, "prepare": True, "landmarker":Path("swappers/hrffa.onnx")}

        def run():
            try:
                with worker:
                    serve_client(worker, FakeEngine(), config)
            except (EOFError, ConnectionError):
                pass
            except Exception as error:
                errors.append(error)

        with patch("face_swap_gpu_worker.GpuImageEngine", factory):
            thread = threading.Thread(target=run)
            thread.start()
            try:
                client.settimeout(3)
                source = b"photo.png"
                client.sendall(GREETING.pack(MAGIC, len(source)) + source)
                magic, size = GREETING.unpack(read_exact(client, GREETING.size))
                message = read_exact(client, size)
                self.assertEqual(magic, MAGIC)
                self.assertIn(b"GPU_MODELS_READY", message)
                # No frame or texture handle has been supplied at this point.
                factory.assert_called_once()
                self.assertIsNone(factory.call_args.args[2])
                self.assertEqual(factory.call_args.kwargs["landmarker_path"],Path("swappers/hrffa.onnx"))
                client.sendall(HEADER.pack(MASK_CONFIG,0,0,28)+struct.pack('!7I',1,25,2,8,12,5,10))
                self.assertEqual(HEADER.unpack(read_exact(client,HEADER.size)),(MASK_CONFIG,0,0,0))
                gpu.configure_mask.assert_called_once_with({'shape':'box','padding':(8,12,5,10),'blur':25,'preview':2,'face_scale':100,'face_size':100,'alignment':0,'expansion':0,'shape_transfer':0,'shape_reset':0,'animation_strength':100})
                ticket = (1, 2, 3)
                client.sendall(HEADER.pack(GPU_MAGIC, 64, 48, 24) + struct.pack("!3Q", *ticket))
                self.assertEqual(HEADER.unpack(read_exact(client, HEADER.size)), (GPU_MAGIC, 64, 48, 0))
                factory.assert_called_once()
                gpu.replace_textures.assert_called_once_with(ticket, 64, 48)
            finally:
                client.close()
                thread.join(4)
            self.assertFalse(thread.is_alive())
            self.assertFalse(errors, errors)
            gpu.close.assert_called_once()

    def test_bounds(self):
        self.assertEqual(frame_header(HEADER.pack(MAGIC, 64, 48, 9216)), (64, 48, 9216))
        self.assertEqual(frame_header(HEADER.pack(MAGIC, 1920, 1080, 6220800)), (1920, 1080, 6220800))
        for values in [(0, 64, 48, 9216), (MAGIC, 1922, 2, 11532),
                       (MAGIC, 0, 48, 0), (MAGIC, 63, 48, 9072), (MAGIC, 64, 48, 1)]:
            with self.assertRaises(ValueError):
                frame_header(HEADER.pack(*values))

    def test_largest_face(self):
        small = SimpleNamespace(bbox=[0, 0, 20, 20])
        large = SimpleNamespace(bbox=[10, 10, 90, 90])
        self.assertIs(largest_face([small, large]), large)
        self.assertIsNone(largest_face([]))

    def test_cuda_required(self):
        session = SimpleNamespace(get_providers=lambda: ["CPUExecutionProvider"])
        with self.assertRaises(RuntimeError):
            require_cuda([SimpleNamespace(session=session)])

    def test_cpu_rejects_gpu_provider(self):
        session = SimpleNamespace(get_providers=lambda: ["CUDAExecutionProvider", "CPUExecutionProvider"])
        with self.assertRaises(RuntimeError):
            require_provider([SimpleNamespace(session=session)], "cpu")

    def test_cpu_accepts_exclusive_cpu_session(self):
        from unittest.mock import Mock
        session = SimpleNamespace(get_providers=lambda: ["CPUExecutionProvider"], disable_fallback=Mock())
        require_provider([SimpleNamespace(session=session)], "cpu")
        session.disable_fallback.assert_called_once()

    def test_directml_configuration(self):
        from unittest.mock import Mock
        options = SimpleNamespace()
        ort = SimpleNamespace(SessionOptions=lambda: options,
            ExecutionMode=SimpleNamespace(ORT_SEQUENTIAL="sequential"),
            get_available_providers=lambda: ["DmlExecutionProvider", "CPUExecutionProvider"])
        providers, configured = execution_config(ort, "directml", 1)
        self.assertIs(configured, options)
        self.assertFalse(options.enable_mem_pattern)
        self.assertEqual(options.execution_mode, "sequential")
        self.assertEqual(providers[0], ("DmlExecutionProvider", {"device_id": 1}))
        session = SimpleNamespace(get_providers=lambda: ["DmlExecutionProvider", "CPUExecutionProvider"],disable_fallback=Mock())
        require_provider([SimpleNamespace(session=session)], "directml")
        session.disable_fallback.assert_called_once()
        session.get_providers=lambda: ["CPUExecutionProvider"]
        with self.assertRaises(RuntimeError):
            require_provider([SimpleNamespace(session=session)], "directml")
        ort.get_available_providers=lambda: ["CPUExecutionProvider"]
        with self.assertRaisesRegex(RuntimeError,"DirectML provider unavailable"):
            execution_config(ort,"directml",0)

    def test_roundtrip_and_source_error(self):
        for source in ("photo-é.png", "reject"):
            client, worker = socket.socketpair()
            errors = []

            def run():
                with worker:
                    try:
                        serve_client(worker, FakeEngine())
                    except EOFError:
                        pass
                    except Exception as error:
                        errors.append(error)

            thread = threading.Thread(target=run)
            thread.start()
            try:
                client.settimeout(2)
                path = source.encode("utf-8")
                # Deliberately fragment configuration writes.
                for byte in GREETING.pack(MAGIC, len(path)) + path:
                    client.sendall(bytes([byte]))
                magic, length = GREETING.unpack(read_exact(client, 8))
                message = read_exact(client, length)
                if source == "reject":
                    self.assertEqual(magic, 0)
                    self.assertIn(b"No face", message)
                else:
                    self.assertEqual(magic, MAGIC)
                    payload = bytes(range(36))
                    header = HEADER.pack(MAGIC, 2, 6, len(payload))
                    client.sendall(header + payload)
                    self.assertEqual(read_exact(client, 16 + len(payload)), header + payload)
            finally:
                client.close()
                thread.join(3)
            self.assertFalse(thread.is_alive())
            self.assertFalse(errors)

    @unittest.skipUnless(os.environ.get("FACE_SWAP_TEST_EXE"), "Native integration executable not selected")
    def test_native_bridge(self):
        failures = []
        stop = threading.Event()
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(4)
            listener.settimeout(0.1)

            def run():
                while not stop.is_set():
                    try:
                        connection, _ = listener.accept()
                    except socket.timeout:
                        continue
                    with connection:
                        connection.settimeout(3)
                        try:
                            magic, length = GREETING.unpack(read_exact(connection, 8))
                            source = read_exact(connection, length).decode()
                            if magic != MAGIC:
                                raise ValueError("Bad native greeting")
                            if source == "reject":
                                connection.sendall(GREETING.pack(0, 8) + b"Rejected")
                                continue
                            connection.sendall(GREETING.pack(MAGIC, 4) + b"CUDA")
                            if source == "stall":
                                while connection.recv(4096):
                                    pass
                                continue
                            while True:
                                header = read_exact(connection, 16)
                                _, _, size = frame_header(header)
                                data = read_exact(connection, size)
                                if source == "malformed":
                                    connection.sendall(HEADER.pack(MAGIC, 8192, 8192, 0))
                                    break
                                connection.sendall(header + data)
                        except (EOFError, ConnectionError):
                            pass
                        except Exception as error:
                            failures.append(error)

            thread = threading.Thread(target=run)
            thread.start()
            try:
                result = subprocess.run([os.environ["FACE_SWAP_TEST_EXE"], str(listener.getsockname()[1])], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            finally:
                stop.set()
                thread.join(4)
            self.assertFalse(thread.is_alive())
            self.assertFalse(failures, failures)

    @unittest.skipUnless(os.environ.get("FACE_SWAP_TEST_EXE"), "Native integration executable not selected")
    def test_native_cpu_accepts_slow_frame(self):
        import time
        failures = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(5)

            def run():
                try:
                    connection, _ = listener.accept()
                    with connection:
                        connection.settimeout(3)
                        magic, length = GREETING.unpack(read_exact(connection, 8))
                        read_exact(connection, length)
                        connection.sendall(GREETING.pack(MAGIC, 9) + b"CPU ready")
                        header = read_exact(connection, 16)
                        _, _, size = frame_header(header)
                        data = read_exact(connection, size)
                        time.sleep(.7)  # Slower than the GPU freshness deadline.
                        connection.sendall(header + data)
                        while connection.recv(4096):
                            pass
                except Exception as error:
                    failures.append(error)

            thread = threading.Thread(target=run)
            thread.start()
            try:
                result = subprocess.run([os.environ["FACE_SWAP_TEST_EXE"], str(listener.getsockname()[1]), "--cpu"], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            finally:
                thread.join(5)
            self.assertFalse(thread.is_alive())
            self.assertFalse(failures, failures)

    @unittest.skipUnless(os.environ.get("FACE_SWAP_TEST_EXE"), "Native integration executable not selected")
    def test_native_result_wakes_without_new_input(self):
        import time
        errors=[]
        class SlowEngine(FakeEngine):
            def swap(self,data,width,height):
                time.sleep(.05)
                return data
        with socket.socket() as listener:
            listener.bind(("127.0.0.1",0));listener.listen(1);listener.settimeout(5)
            def run():
                try:
                    connection,_=listener.accept()
                    with connection:
                        serve_client(connection,SlowEngine())
                except (EOFError,ConnectionError):
                    pass
                except Exception as error:
                    errors.append(error)
            thread=threading.Thread(target=run);thread.start()
            try:
                result=subprocess.run([os.environ["FACE_SWAP_TEST_EXE"],str(listener.getsockname()[1]),"--wake-test"],capture_output=True,text=True,timeout=8)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            finally:
                thread.join(5)
            self.assertFalse(thread.is_alive())
            self.assertFalse(errors,errors)

    @unittest.skipUnless(os.environ.get("FACE_SWAP_TEST_EXE"), "Native integration executable not selected")
    def test_native_disconnect_keeps_preparation_and_reuses_worker(self):
        import time
        errors=[]
        class SlowEngine(FakeEngine):
            def swap(self,data,width,height):
                time.sleep(.35)
                return data
        with socket.socket() as listener:
            listener.bind(("127.0.0.1",0));listener.listen(1);listener.settimeout(5)
            def run():
                try:
                    connection,_=listener.accept()
                    with connection:
                        serve_client(connection,SlowEngine())
                except (EOFError,ConnectionError):
                    pass
                except Exception as error:
                    errors.append(error)
            thread=threading.Thread(target=run);thread.start()
            try:
                result=subprocess.run([os.environ["FACE_SWAP_TEST_EXE"],str(listener.getsockname()[1]),"--detach-test"],capture_output=True,text=True,timeout=8)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            finally:
                thread.join(5)
            self.assertFalse(thread.is_alive())
            self.assertFalse(errors,errors)

    @unittest.skipUnless(os.environ.get("FACE_SWAP_TEST_EXE"), "Native integration executable not selected")
    def test_native_cadence_metrics(self):
        result = subprocess.run([os.environ["FACE_SWAP_TEST_EXE"], "--test-cadence"],capture_output=True,text=True,timeout=5)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_gpu_control_and_pixel_fallback(self):
        for reject in (False, True):
            created, runs, errors = [], [], []
            resizes = []

            class FakeGpu:
                def __init__(self, *args):
                    if reject:
                        raise RuntimeError("Interop unavailable")
                    created.append(1)
                    self.last_face_count, self.metadata_bytes = 1, 180

                def process(self):
                    runs.append(1)
                    return 1.0

                def close(self):
                    pass

                def replace_textures(self, ticket, width, height):
                    resizes.append((width, height))
                    return True

            client, worker = socket.socketpair()
            config = dict(dll=None, cache=None, restorer=None, strength=.6)

            def run():
                with worker:
                    try:
                        serve_client(worker, FakeEngine(), config)
                    except EOFError:
                        pass
                    except Exception as error:
                        errors.append(error)

            with patch.dict(sys.modules, {"face_swap_gpu_worker": SimpleNamespace(GpuImageEngine=FakeGpu)}), redirect_stderr(io.StringIO()):
                thread = threading.Thread(target=run)
                thread.start()
                try:
                    client.settimeout(2)
                    client.sendall(GREETING.pack(MAGIC, 6) + b"normal")
                    magic, length = GREETING.unpack(read_exact(client, 8))
                    self.assertEqual(magic, MAGIC)
                    self.assertIn(b"GPU_TEXTURES", read_exact(client, length))
                    packet = HEADER.pack(GPU_MAGIC, 64, 48, 24) + struct.pack("!3Q", 1, 2, 3)
                    client.sendall(packet)
                    response = HEADER.unpack(read_exact(client, 16))
                    if reject:
                        self.assertEqual(response[0], GPU_ERROR)
                        self.assertIn(b"Interop unavailable", read_exact(client, response[3]))
                        payload = bytes(range(36))
                        header = HEADER.pack(MAGIC, 2, 6, len(payload))
                        client.sendall(header + payload)
                        self.assertEqual(read_exact(client, 52), header + payload)
                    else:
                        self.assertEqual(response, (GPU_MAGIC, 64, 48, 0))
                        client.sendall(packet)
                        self.assertEqual(HEADER.unpack(read_exact(client, 16)), (GPU_MAGIC, 64, 48, 0))
                        self.assertEqual(len(created), 1)
                        self.assertEqual(len(runs), 2)
                        client.sendall(HEADER.pack(GPU_MAGIC, 96, 48, 24) + struct.pack("!3Q", 4, 5, 3))
                        self.assertEqual(HEADER.unpack(read_exact(client, 16)), (GPU_MAGIC, 96, 48, 0))
                        self.assertEqual(resizes, [(96, 48)])
                        self.assertEqual(len(created), 1)
                finally:
                    client.close()
                    thread.join(3)
            self.assertFalse(thread.is_alive())
            self.assertFalse(errors, errors)

    @unittest.skipUnless(os.name == "nt" and os.environ.get("FACE_SWAP_TEST_EXE"), "Windows native executable required")
    def test_managed_process_lifetime(self):
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        with tempfile.TemporaryDirectory(prefix="face swap \u00e9 ") as folder:
            directory = Path(folder)
            for name in ("det_10g.onnx", "w600k_r50.onnx", "inswapper_128.fp16.onnx", "GPEN-BFR-256.onnx"):
                (directory / name).touch()
            command = [os.environ["FACE_SWAP_TEST_EXE"], "0", sys.executable,
                       str(Path(__file__).with_name("fixture_face_swap_worker.py").resolve()), str(directory)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=25)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse(list(directory.rglob("*.port")), "Readiness files left behind after normal stop")
            levels=list(directory.rglob("*.builder"))
            self.assertTrue(levels,"Worker did not record selected TensorRT level")
            self.assertTrue(all(file.read_text()=="3" for file in levels),"Global level did not reach InSwapper/GPEN worker")
            # The job must also terminate its worker when the native process
            # exits without running C++ destructors (crash-equivalent cleanup).
            result = subprocess.run(command + ["--crash"], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            pids = list(directory.rglob("*.pid"))
            self.assertGreaterEqual(len(pids), 4)
            for file in pids:
                process = kernel.OpenProcess(0x100000, False, int(file.read_text()))
                if process:
                    try:
                        self.assertEqual(kernel.WaitForSingleObject(process, 2000), 0, "Owned worker left running")
                    finally:
                        kernel.CloseHandle(process)


if __name__ == "__main__":
    unittest.main()
