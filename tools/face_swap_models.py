"""Adapters for explicitly selected local 256-pixel swapper models.

Model IO conventions are documented by the model publishers / FaceFusion.
This module implements our own preprocessing and paste-back; it does not
download weights or change the chosen identity when a backend fails.
"""
from pathlib import Path


MODEL_NAMES = ("inswapper_128", "alphaface_256", "hyperswap_1a_256", "hyperswap_1b_256", "hyperswap_1c_256")


def prepare_model(name, path, cache, backend="cuda"):
    """Prepare backend-specific graphs without editing original model weights."""
    if backend == "directml" and name in MODEL_NAMES[2:]:
        return prepare_directml_hyperswap(name, path, cache)
    if name != "alphaface_256":
        return str(path)
    import hashlib
    import onnx
    from onnxruntime.tools.symbolic_shape_infer import SymbolicShapeInference
    key = hashlib.sha256(Path(path).read_bytes()).hexdigest()[:24]
    target = Path(cache) / (name+"-"+key+"-static-v1.onnx")
    if not target.is_file():
        model = onnx.load(str(path), load_external_data=False)
        for value in model.graph.input:
            dim = value.type.tensor_type.shape.dim[0]
            dim.ClearField("dim_param")
            dim.dim_value = 1
        # This export incorrectly aliases its height to the batch dimension.
        shape = model.graph.output[0].type.tensor_type.shape
        shape.ClearField("dim")
        for size in (1, 3, 256, 256):
            shape.dim.add().dim_value = size
        model = SymbolicShapeInference.infer_shapes(model, auto_merge=True)
        target.parent.mkdir(parents=True, exist_ok=True)
        from uuid import uuid4
        temporary = target.with_name(target.name+"."+uuid4().hex+".tmp")
        try:
            onnx.save(model, str(temporary))
            temporary.replace(target)
        finally:
            temporary.unlink(missing_ok=True)
    return str(target)


def prepare_directml_hyperswap(name, path, cache):
    """Avoid DirectML's inaccurate FP16 HyperSwap evaluation with a FP32 copy.

    Promote computation, not just IO: the published graph casts its float32
    inputs to half and stores its weights/constants in half. Changing external
    tensor types alone leaves the faulty half-precision calculation intact.
    CUDA/TensorRT and the original downloaded model are unaffected.
    """
    import hashlib
    import numpy as np
    import onnx
    from onnx import AttributeProto, TensorProto, numpy_helper
    from uuid import uuid4
    with Path(path).open("rb") as source:
        key = hashlib.file_digest(source, "sha256").hexdigest()[:24]
    target = Path(cache) / (name+"-"+key+"-directml-fp32-v1.onnx")
    if not target.is_file():
        model = onnx.load(str(path))

        def tensor(value):
            if value.data_type == TensorProto.FLOAT16:
                value.CopyFrom(numpy_helper.from_array(
                    numpy_helper.to_array(value).astype(np.float32), value.name))

        def graph(value):
            for item in value.initializer:
                tensor(item)
            for item in list(value.input)+list(value.output)+list(value.value_info):
                if item.type.HasField("tensor_type") and item.type.tensor_type.elem_type == TensorProto.FLOAT16:
                    item.type.tensor_type.elem_type = TensorProto.FLOAT
            for node in value.node:
                for attribute in node.attribute:
                    if attribute.type == AttributeProto.TENSOR:
                        tensor(attribute.t)
                    elif attribute.type == AttributeProto.TENSORS:
                        for item in attribute.tensors:
                            tensor(item)
                    elif attribute.type == AttributeProto.GRAPH:
                        graph(attribute.g)
                    elif attribute.type == AttributeProto.GRAPHS:
                        for item in attribute.graphs:
                            graph(item)
                    if node.op_type == "Cast" and attribute.name == "to" and attribute.i == TensorProto.FLOAT16:
                        attribute.i = TensorProto.FLOAT

        graph(model.graph)
        onnx.checker.check_model(model)
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_name(target.name+"."+uuid4().hex+".tmp")
        try:
            onnx.save(model, str(temporary))
            temporary.replace(target)
        finally:
            temporary.unlink(missing_ok=True)
    print("HyperSwap: DirectML FP32 precision correction (cached graph)", flush=True)
    return str(target)


def identity_tensor(name, source, original=None):
    import numpy as np
    if name == "inswapper_128":
        latent = np.dot(source.normed_embedding.reshape(1, -1), original.emap)
        latent /= np.linalg.norm(latent)
    elif name == "alphaface_256":
        latent = source.embedding.reshape(1, -1)
    elif name.startswith("hyperswap_") and name in MODEL_NAMES:
        latent = source.normed_embedding.reshape(1, -1)
    else:
        raise ValueError("Unsupported face swapper: " + name)
    return np.ascontiguousarray(latent, dtype=np.float32)


def image_convention(name):
    # Input mean/scale, then output scale/bias in byte coordinates.
    return (127.5, 1 / 127.5, 127.5, 127.5) if name.startswith("hyperswap_") else (0, 1 / 255, 255, 0)


def estimate_face_matrix(landmarks, side, alignment=0):
    """Optional FaceFusion-style ArcFace128 fit, with the existing detector points.

    Convention reference: facefusion/face_helper.py, estimate_matrix_by_face_landmark_5.
    This intentionally does not introduce a 68-point model or change source identity.
    """
    if type(alignment) is not int or alignment not in (0,1):
        raise ValueError('Alignment must be 0 or 1')
    if alignment==0:
        from insightface.utils.face_align import estimate_norm
        return estimate_norm(landmarks,image_size=side)
    import cv2
    import numpy as np
    points=np.asarray(landmarks,dtype=np.float32)
    if points.shape!=(5,2) or not np.isfinite(points).all():
        raise ValueError('Expected five finite detector landmarks')
    # Normalized ArcFace128 target coordinates used by all supported swappers.
    target=np.array([[.36167656,.40387734],[.63696719,.40235469],
                     [.50019687,.56044219],[.38710391,.72160547],
                     [.61507734,.72034453]],dtype=np.float64)*side
    matrix,_=cv2.estimateAffinePartial2D(points,target,method=cv2.RANSAC,ransacReprojThreshold=100)
    if matrix is None or not np.isfinite(matrix).all() or np.linalg.det(matrix[:,:2])<=1e-12:
        raise ValueError('Cannot align degenerate face landmarks')
    return matrix


class AlternativeSwapper:
    """Host pixel transport adapter for CUDA or CPU model execution."""
    def __init__(self, name, path, providers, options):
        import onnxruntime as ort
        self.name, self.model_file = name, str(path)
        if name not in MODEL_NAMES[1:]:
            raise ValueError("Unsupported alternative swapper")
        self.session = ort.InferenceSession(str(path), providers=providers, sess_options=options)
        self.session.disable_fallback()
        self.input_size = (256, 256)
        inputs = self.session.get_inputs()
        self.image_name = next(item.name for item in inputs if len(item.shape) == 4)
        self.identity_name = next(item.name for item in inputs if len(item.shape) == 2)

    def get(self, image, face, source, paste_back=True):
        import cv2
        import numpy as np
        from insightface.utils.face_align import norm_crop2
        side = self.input_size[0]
        crop, matrix = norm_crop2(image, face.kps, side)
        mean, scale, output_scale, bias = image_convention(self.name)
        tensor = np.ascontiguousarray((crop[:, :, ::-1].transpose(2, 0, 1)[None].astype(np.float32)-mean)*scale)
        pixels = self.session.run(None, {self.image_name: tensor, self.identity_name: identity_tensor(self.name, source)})[0]
        rgb = np.clip(pixels[0].transpose(1, 2, 0)*output_scale+bias, 0, 255)
        bgr = np.ascontiguousarray(rgb[:, :, ::-1], dtype=np.uint8)
        if not paste_back:
            return bgr, matrix
        y, x = np.mgrid[:side, :side].astype(np.float32)
        radius = np.sqrt(((x-(side-1)/2)/(side*.46))**2+((y-(side-1)/2)/(side*.49))**2)
        mask = np.clip((1-radius)/.18, 0, 1)
        mask = mask*mask*(3-2*mask)
        inverse = cv2.invertAffineTransform(matrix)
        size = (image.shape[1], image.shape[0])
        swapped = cv2.warpAffine(bgr, inverse, size)
        alpha = cv2.warpAffine(mask, inverse, size)[:, :, None]
        return np.clip(image*(1-alpha)+swapped*alpha, 0, 255).astype(np.uint8)
