# Rewrites BiRefNet's exported deformable convolutions (GatherND decomposition from
# deform_conv2d_onnx_exporter) into GridSample + 1x1 Conv, which DirectML runs fast.
#   python fixdeform.py in.onnx out.onnx
import sys, numpy as np, onnx
from onnx import helper as h, numpy_helper as nh, shape_inference, TensorProto as T

src, dst = sys.argv[1], sys.argv[2]
m = onnx.load(src)
g = m.graph
inf = shape_inference.infer_shapes(m)
shape = {v.name: [d.dim_value for d in v.type.tensor_type.shape.dim] for v in inf.graph.value_info}
for v in list(g.input):
    shape[v.name] = [d.dim_value for d in v.type.tensor_type.shape.dim]
inits = {i.name: i for i in g.initializer}
consts = {n.output[0]: n for n in g.node if n.op_type == "Constant"}

allnodes = list(g.node)
idx = {n.name: k for k, n in enumerate(allnodes)}
scopes = sorted({n.name.rsplit("/", 1)[0] + "/" for n in g.node if n.name.endswith("atrous_conv/GatherND")})
print("deform scopes:", len(scopes))
new_inits = []
uid = [0]

def name(s):
    uid[0] += 1
    return f"dcfix_{uid[0]}_{s}"

def const(arr, n):
    t = nh.from_array(arr, name(n))
    new_inits.append(t)
    return t.name

replace = {}   # index of regular conv node -> list of new nodes
remove = set()
for sc in scopes:
    ns = [n for n in allnodes if n.name.startswith(sc)]
    convs = [n for n in ns if n.op_type == "Conv"]
    off = next(n for n in convs if "offset_conv.weight" in n.input[1])
    mod = next(n for n in convs if "modulator_conv.weight" in n.input[1])
    reg = next(n for n in convs if "regular_conv.weight" in n.input[1])
    sig = next(n for n in ns if n.op_type == "Sigmoid" and n.input[0] == mod.output[0])
    mmul = next(n for n in ns if n.op_type == "Mul" and sig.output[0] in n.input)
    keep = {off.name, mod.name, sig.name, mmul.name}
    # nodes after the regular conv (bias Add) stay
    ridx = idx[reg.name]
    for n in ns:
        if n.name not in keep and n.op_type != "Constant" and idx[n.name] < ridx:
            remove.add(idx[n.name])
    X = off.input[0]
    _, C, H, W = shape[X]
    w = inits[reg.input[1]]
    wa = nh.to_array(w)
    Cout, Cw, kh, kw = wa.shape
    assert Cw == C, (sc, Cw, C)
    K = kh * kw
    p = (kh - 1) // 2
    sx, sy = 2.0 / (W - 1), 2.0 / (H - 1)
    jj = np.array([j for i in range(kh) for j in range(kw)], np.float32)
    ii = np.array([i for i in range(kh) for j in range(kw)], np.float32)
    ck_x = ((jj - p) * sx - 1).reshape(1, K, 1, 1).astype(np.float32)
    ck_y = ((ii - p) * sy - 1).reshape(1, K, 1, 1).astype(np.float32)
    cw_x = (np.arange(W, dtype=np.float32) * sx).reshape(1, 1, 1, W)
    ch_y = (np.arange(H, dtype=np.float32) * sy).reshape(1, 1, H, 1)
    fdt = w.data_type  # fp16 or fp32
    nodes = []
    o5 = name("off5")
    nodes.append(h.make_node("Reshape", [off.output[0], const(np.array([1, K, 2, H, W], np.int64), "s5")], [o5]))
    of32 = name("off32")
    nodes.append(h.make_node("Cast", [o5], [of32], to=T.FLOAT))
    dy, dx = name("dy"), name("dx")
    nodes.append(h.make_node("Split", [of32, const(np.array([1, 1], np.int64), "sp")], [dy, dx], axis=2))
    # dy, dx: [1,K,1,H,W] -> [1,K,H,W]
    dy4, dx4 = name("dy4"), name("dx4")
    ax = const(np.array([2], np.int64), "ax2")
    nodes.append(h.make_node("Squeeze", [dy, ax], [dy4]))
    nodes.append(h.make_node("Squeeze", [dx, ax], [dx4]))
    gx, gy = [name("gx%d" % k) for k in range(3)], [name("gy%d" % k) for k in range(3)]
    nodes.append(h.make_node("Mul", [dx4, const(np.array(sx, np.float32), "sx")], [gx[0]]))
    nodes.append(h.make_node("Add", [gx[0], const(cw_x, "cwx")], [gx[1]]))
    nodes.append(h.make_node("Add", [gx[1], const(ck_x, "ckx")], [gx[2]]))
    nodes.append(h.make_node("Mul", [dy4, const(np.array(sy, np.float32), "sy")], [gy[0]]))
    nodes.append(h.make_node("Add", [gy[0], const(ch_y, "chy")], [gy[1]]))
    nodes.append(h.make_node("Add", [gy[1], const(ck_y, "cky")], [gy[2]]))
    ax4 = const(np.array([4], np.int64), "ax4")
    ux, uy = name("ux"), name("uy")
    nodes.append(h.make_node("Unsqueeze", [gx[2], ax4], [ux]))
    nodes.append(h.make_node("Unsqueeze", [gy[2], ax4], [uy]))
    grid5 = name("grid5")
    nodes.append(h.make_node("Concat", [ux, uy], [grid5], axis=4))
    grid = name("grid")
    nodes.append(h.make_node("Reshape", [grid5, const(np.array([1, K * H, W, 2], np.int64), "sg")], [grid]))
    if fdt == T.FLOAT16:
        grid16 = name("grid16")
        nodes.append(h.make_node("Cast", [grid], [grid16], to=T.FLOAT16))
        grid = grid16
    samp = name("samp")
    nodes.append(h.make_node("GridSample", [X, grid], [samp], mode="bilinear", padding_mode="zeros",
                             align_corners=1))
    mk = name("mask")
    nodes.append(h.make_node("Reshape", [mmul.output[0], const(np.array([1, 1, K * H, W], np.int64), "sm")], [mk]))
    sm = name("sampm")
    nodes.append(h.make_node("Mul", [samp, mk], [sm]))
    col = name("col")
    nodes.append(h.make_node("Reshape", [sm, const(np.array([1, C * K, H, W], np.int64), "sc")], [col]))
    w2 = nh.from_array(wa.reshape(Cout, C * K, 1, 1), name("w"))
    new_inits.append(w2)
    nodes.append(h.make_node("Conv", [col, w2.name] + list(reg.input[2:]), list(reg.output), kernel_shape=[1, 1]))
    replace[ridx] = nodes

out = []
for k, n in enumerate(allnodes):
    if k in replace:
        out.extend(replace[k])
    elif k not in remove:
        out.append(n)
del g.node[:]
g.node.extend(out)
g.initializer.extend(new_inits)

# drop nodes and initializers nothing uses any more
need = {o.name for o in g.output}
alive = []
for n in reversed(list(g.node)):
    if any(o in need for o in n.output):
        alive.append(n)
        need.update(i for i in n.input if i)
alive.reverse()
del g.node[:]
g.node.extend(alive)
keepi = [i for i in g.initializer if i.name in need]
del g.initializer[:]
g.initializer.extend(keepi)
del g.value_info[:]
print("nodes", len(g.node))
onnx.save(m, dst)
