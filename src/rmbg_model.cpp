// Rewrites the BiRefNet ONNX model once after download so it runs fast on the GPU.
//
// BiRefNet uses deformable convolutions. The public ONNX export spells each one out as a big
// GatherND / Transpose / Mul / ReduceSum chain over huge tensors (about 8 billion elements in all),
// which DirectML runs ~10x slower than the rest of the network. Each one is replaced here by the
// same math in two cheap steps: GridSample (bilinear sampling at the learned offsets, scaled by the
// learned mask) followed by a 1x1 convolution with the original weights. Same result to within fp16
// rounding (checked against the original model: mean difference 0.0000, max 0.02 on the mask).
// tools/ai/fixdeform.py is the same rewrite in Python, kept for reference.
//
// ONNX files are protobuf. Only the few messages touched here are decoded (nodes, initializers,
// Constant tensors); everything else is copied byte for byte.
#include "app.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// ---- protobuf reading -----------------------------------------------------------------------
struct Field {
    uint32_t num = 0, wire = 0;
    uint64_t v = 0;                 // varint / fixed value
    const uint8_t* p = nullptr;     // length-delimited payload
    size_t n = 0;
    const uint8_t* raw = nullptr;   // the whole field, tag included, for copying unchanged
    size_t rawLen = 0;
    std::string str() const { return std::string(reinterpret_cast<const char*>(p), n); }
};

bool ReadVarint(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (p >= end) return false;
        const uint8_t b = *p++;
        v |= static_cast<uint64_t>(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

bool Parse(const uint8_t* p, size_t n, std::vector<Field>& out) {
    const uint8_t* end = p + n;
    while (p < end) {
        Field f;
        f.raw = p;
        uint64_t tag;
        if (!ReadVarint(p, end, tag)) return false;
        f.num = static_cast<uint32_t>(tag >> 3);
        f.wire = static_cast<uint32_t>(tag & 7);
        switch (f.wire) {
        case 0:
            if (!ReadVarint(p, end, f.v)) return false;
            break;
        case 1:
            if (end - p < 8) return false;
            memcpy(&f.v, p, 8);
            p += 8;
            break;
        case 2: {
            uint64_t len;
            if (!ReadVarint(p, end, len) || len > static_cast<uint64_t>(end - p)) return false;
            f.p = p;
            f.n = static_cast<size_t>(len);
            p += len;
            break;
        }
        case 5:
            if (end - p < 4) return false;
            memcpy(&f.v, p, 4);
            p += 4;
            break;
        default:
            return false;
        }
        f.rawLen = static_cast<size_t>(p - f.raw);
        out.push_back(f);
    }
    return true;
}

// Repeated int64 field, packed or not.
void Int64s(const std::vector<Field>& fs, uint32_t num, std::vector<int64_t>& out) {
    for (const Field& f : fs) {
        if (f.num != num) continue;
        if (f.wire == 0) {
            out.push_back(static_cast<int64_t>(f.v));
        } else if (f.wire == 2) {
            const uint8_t* q = f.p;
            uint64_t v;
            while (q < f.p + f.n && ReadVarint(q, f.p + f.n, v)) out.push_back(static_cast<int64_t>(v));
        }
    }
}

// ---- protobuf writing -----------------------------------------------------------------------
struct Buf {
    std::string s;
    void Varint(uint64_t v) {
        while (v >= 0x80) {
            s.push_back(static_cast<char>((v & 0x7f) | 0x80));
            v >>= 7;
        }
        s.push_back(static_cast<char>(v));
    }
    void Tag(uint32_t num, uint32_t wire) { Varint((static_cast<uint64_t>(num) << 3) | wire); }
    void Int(uint32_t num, int64_t v) {
        Tag(num, 0);
        Varint(static_cast<uint64_t>(v));
    }
    void Bytes(uint32_t num, const void* p, size_t n) {
        Tag(num, 2);
        Varint(n);
        s.append(static_cast<const char*>(p), n);
    }
    void Str(uint32_t num, const std::string& v) { Bytes(num, v.data(), v.size()); }
    void Raw(const uint8_t* p, size_t n) { s.append(reinterpret_cast<const char*>(p), n); }
};

// ONNX field numbers
enum : uint32_t {
    kModelGraph = 7,
    kGraphNode = 1, kGraphInit = 5, kGraphOutput = 12, kGraphValueInfo = 13,
    kNodeIn = 1, kNodeOut = 2, kNodeName = 3, kNodeOp = 4, kNodeAttr = 5,
    kAttrName = 1, kAttrI = 3, kAttrS = 4, kAttrT = 5, kAttrInts = 8, kAttrType = 20,
    kTensorDims = 1, kTensorType = 2, kTensorInt64 = 7, kTensorName = 8, kTensorRaw = 9,
};
enum : int { kAttrTypeInt = 2, kAttrTypeString = 3, kAttrTypeInts = 7 };
enum : int { kFloat = 1, kInt64 = 7, kFloat16 = 10 };

struct Node {
    std::vector<std::string> in, out;
    std::string name, op;
    std::vector<Field> attrs;          // AttributeProto payloads (for Constant values)
    const uint8_t* raw = nullptr;      // payload of the original NodeProto
    size_t rawLen = 0;
    std::string built;                 // payload of a new node
};

struct Init {
    std::string name;
    std::vector<int64_t> dims;
    int type = 0;
    const uint8_t* data = nullptr;     // raw_data
    size_t dataLen = 0;
    const uint8_t* raw = nullptr;      // payload of the original TensorProto
    size_t rawLen = 0;
    std::string built;
};

bool ParseNode(const uint8_t* p, size_t n, Node& node) {
    std::vector<Field> fs;
    if (!Parse(p, n, fs)) return false;
    node.raw = p;
    node.rawLen = n;
    for (const Field& f : fs) {
        if (f.wire != 2) continue;
        if (f.num == kNodeIn) node.in.push_back(f.str());
        else if (f.num == kNodeOut) node.out.push_back(f.str());
        else if (f.num == kNodeName) node.name = f.str();
        else if (f.num == kNodeOp) node.op = f.str();
        else if (f.num == kNodeAttr) node.attrs.push_back(f);
    }
    return true;
}

bool ParseInit(const uint8_t* p, size_t n, Init& t) {
    std::vector<Field> fs;
    if (!Parse(p, n, fs)) return false;
    t.raw = p;
    t.rawLen = n;
    Int64s(fs, kTensorDims, t.dims);
    for (const Field& f : fs) {
        if (f.num == kTensorType && f.wire == 0) t.type = static_cast<int>(f.v);
        else if (f.num == kTensorName && f.wire == 2) t.name = f.str();
        else if (f.num == kTensorRaw && f.wire == 2) {
            t.data = f.p;
            t.dataLen = f.n;
        }
    }
    return true;
}

// int64 values of a Constant node's "value" tensor
bool ConstInts(const Node& n, std::vector<int64_t>& out) {
    for (const Field& a : n.attrs) {
        std::vector<Field> af;
        if (!Parse(a.p, a.n, af)) return false;
        std::string name;
        const Field* t = nullptr;
        for (const Field& f : af) {
            if (f.num == kAttrName && f.wire == 2) name = f.str();
            if (f.num == kAttrT && f.wire == 2) t = &f;
        }
        if (name != "value" || !t) continue;
        std::vector<Field> tf;
        if (!Parse(t->p, t->n, tf)) return false;
        int type = 0;
        for (const Field& f : tf)
            if (f.num == kTensorType && f.wire == 0) type = static_cast<int>(f.v);
        if (type != kInt64) return false;
        for (const Field& f : tf)
            if (f.num == kTensorRaw && f.wire == 2) {
                for (size_t i = 0; i + 8 <= f.n; i += 8) {
                    int64_t v;
                    memcpy(&v, f.p + i, 8);
                    out.push_back(v);
                }
                return true;
            }
        Int64s(tf, kTensorInt64, out);
        return true;
    }
    return false;
}

std::string TensorBytes(const std::string& name, int type, const std::vector<int64_t>& dims, const void* data,
                        size_t len) {
    Buf b;
    for (int64_t d : dims) b.Int(kTensorDims, d);
    b.Int(kTensorType, type);
    b.Str(kTensorName, name);
    b.Bytes(kTensorRaw, data, len);
    return b.s;
}

struct Attr {
    std::string name;
    int kind;  // kAttrType*
    int64_t i = 0;
    std::string s;
    std::vector<int64_t> ints;
};
Attr AInt(const char* n, int64_t v) { return {n, kAttrTypeInt, v, {}, {}}; }
Attr AStr(const char* n, const char* v) { return {n, kAttrTypeString, 0, v, {}}; }
Attr AInts(const char* n, std::vector<int64_t> v) { return {n, kAttrTypeInts, 0, {}, std::move(v)}; }

class Rewriter {
public:
    std::vector<Node> newNodes;
    std::vector<Init> newInits;

    std::string Name(const char* s) { return "dcfix_" + std::to_string(++uid_) + "_" + s; }

    std::string ConstF(const char* n, std::vector<int64_t> dims, const std::vector<float>& v) {
        Init t;
        t.name = Name(n);
        t.built = TensorBytes(t.name, kFloat, dims, v.data(), v.size() * 4);
        newInits.push_back(std::move(t));
        return newInits.back().name;
    }
    std::string ConstI(const char* n, const std::vector<int64_t>& v) {
        Init t;
        t.name = Name(n);
        t.built = TensorBytes(t.name, kInt64, {static_cast<int64_t>(v.size())}, v.data(), v.size() * 8);
        newInits.push_back(std::move(t));
        return newInits.back().name;
    }
    void Add(const char* op, std::vector<std::string> in, std::vector<std::string> out, std::vector<Attr> attrs = {}) {
        Node n;
        n.op = op;
        n.name = Name(op);
        n.in = std::move(in);
        n.out = std::move(out);
        Buf b;
        for (const std::string& s : n.in) b.Str(kNodeIn, s);
        for (const std::string& s : n.out) b.Str(kNodeOut, s);
        b.Str(kNodeName, n.name);
        b.Str(kNodeOp, n.op);
        for (const Attr& a : attrs) {
            Buf ab;
            ab.Str(kAttrName, a.name);
            if (a.kind == kAttrTypeInt) ab.Int(kAttrI, a.i);
            else if (a.kind == kAttrTypeString) ab.Str(kAttrS, a.s);
            else
                for (int64_t v : a.ints) ab.Int(kAttrInts, v);
            ab.Int(kAttrType, a.kind);
            b.Str(kNodeAttr, ab.s);
        }
        n.built = std::move(b.s);
        newNodes.push_back(std::move(n));
    }

private:
    int uid_ = 0;
};

bool WriteAll(HANDLE f, const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n) {
        const DWORD chunk = static_cast<DWORD>(n > (64u << 20) ? (64u << 20) : n);
        DWORD wrote = 0;
        if (!WriteFile(f, c, chunk, &wrote, nullptr) || wrote != chunk) return false;
        c += chunk;
        n -= chunk;
    }
    return true;
}

bool EndsWith(const std::string& s, const char* t) {
    const size_t n = strlen(t);
    return s.size() >= n && s.compare(s.size() - n, n, t) == 0;
}

}  // namespace

bool Rmbg_FixModel(const std::wstring& src, const std::wstring& dst, std::wstring& err) {
    HANDLE f = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        err = L"Couldn't open the downloaded model.";
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const uint8_t* base = map ? static_cast<const uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0)) : nullptr;
    struct Cleanup {
        HANDLE f, map;
        const uint8_t* base;
        ~Cleanup() {
            if (base) UnmapViewOfFile(base);
            if (map) CloseHandle(map);
            CloseHandle(f);
        }
    } cleanup{f, map, base};
    if (!base) {
        err = L"Couldn't read the downloaded model.";
        return false;
    }
    err = L"The downloaded model isn't the expected one.";

    std::vector<Field> model, graph;
    if (!Parse(base, static_cast<size_t>(size.QuadPart), model)) return false;
    const Field* gf = nullptr;
    for (const Field& x : model)
        if (x.num == kModelGraph && x.wire == 2) gf = &x;
    if (!gf || !Parse(gf->p, gf->n, graph)) return false;

    std::vector<Node> nodes;
    std::vector<Init> inits;
    std::vector<std::string> outputs;
    for (const Field& x : graph) {
        if (x.wire != 2) continue;
        if (x.num == kGraphNode) {
            nodes.emplace_back();
            if (!ParseNode(x.p, x.n, nodes.back())) return false;
        } else if (x.num == kGraphInit) {
            inits.emplace_back();
            if (!ParseInit(x.p, x.n, inits.back())) return false;
        } else if (x.num == kGraphOutput) {
            std::vector<Field> vf;
            if (!Parse(x.p, x.n, vf)) return false;
            for (const Field& v : vf)
                if (v.num == 1 && v.wire == 2) outputs.push_back(v.str());
        }
    }

    std::unordered_map<std::string, size_t> initIdx, constIdx;
    for (size_t i = 0; i < inits.size(); ++i) initIdx[inits[i].name] = i;
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].op == "Constant" && !nodes[i].out.empty()) constIdx[nodes[i].out[0]] = i;
    std::set<std::string> scopes;
    for (const Node& n : nodes)
        if (EndsWith(n.name, "atrous_conv/GatherND")) scopes.insert(n.name.substr(0, n.name.rfind('/') + 1));
    if (scopes.empty()) return false;

    Rewriter rw;
    std::map<size_t, std::pair<size_t, size_t>> replace;  // regular conv index -> range in rw.newNodes
    std::unordered_set<size_t> remove;
    for (const std::string& sc : scopes) {
        std::vector<size_t> ns;
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].name.compare(0, sc.size(), sc) == 0) ns.push_back(i);
        auto conv = [&](const char* w) -> const Node* {
            for (size_t i : ns)
                if (nodes[i].op == "Conv" && nodes[i].in.size() > 1 && nodes[i].in[1].find(w) != std::string::npos)
                    return &nodes[i];
            return nullptr;
        };
        const Node *off = conv("offset_conv.weight"), *mod = conv("modulator_conv.weight"),
                   *reg = conv("regular_conv.weight");
        if (!off || !mod || !reg) return false;
        const Node *sig = nullptr, *mmul = nullptr, *offReshape = nullptr;
        for (size_t i : ns)
            if (nodes[i].op == "Sigmoid" && nodes[i].in[0] == mod->out[0]) sig = &nodes[i];
        if (!sig) return false;
        for (size_t i : ns) {
            const Node& n = nodes[i];
            if (n.op == "Mul" && (n.in[0] == sig->out[0] || (n.in.size() > 1 && n.in[1] == sig->out[0]))) mmul = &n;
            if (n.op == "Reshape" && n.in[0] == off->out[0]) offReshape = &n;
        }
        if (!mmul || !offReshape || offReshape->in.size() < 2 || !constIdx.count(offReshape->in[1])) return false;
        std::vector<int64_t> s6;  // [1, 1, K, 2, H, W]
        if (!ConstInts(nodes[constIdx[offReshape->in[1]]], s6) || s6.size() != 6) return false;
        const int64_t K = s6[2], H = s6[4], W = s6[5];
        auto wit = initIdx.find(reg->in[1]);
        if (wit == initIdx.end()) return false;
        const Init& w = inits[wit->second];
        if (w.dims.size() != 4 || w.dims[2] * w.dims[3] != K || !w.data || H < 2 || W < 2) return false;
        const int64_t Cout = w.dims[0], C = w.dims[1], kh = w.dims[2], kw = w.dims[3];
        const size_t ridx = static_cast<size_t>(reg - nodes.data());
        for (size_t i : ns)
            if (&nodes[i] != off && &nodes[i] != mod && &nodes[i] != sig && &nodes[i] != mmul &&
                nodes[i].op != "Constant" && i < ridx)
                remove.insert(i);

        // grid = base position of each kernel tap + learned offset, in GridSample's [-1, 1] units
        const float sx = 2.0f / static_cast<float>(W - 1), sy = 2.0f / static_cast<float>(H - 1);
        const int64_t p = (kh - 1) / 2;
        std::vector<float> ckx, cky, cwx, chy;
        for (int64_t i = 0; i < kh; ++i)
            for (int64_t j = 0; j < kw; ++j) {
                ckx.push_back(static_cast<float>(j - p) * sx - 1.0f);
                cky.push_back(static_cast<float>(i - p) * sy - 1.0f);
            }
        for (int64_t x = 0; x < W; ++x) cwx.push_back(static_cast<float>(x) * sx);
        for (int64_t y = 0; y < H; ++y) chy.push_back(static_cast<float>(y) * sy);
        const bool half = w.type == kFloat16;
        const size_t first = rw.newNodes.size();

        const std::string o5 = rw.Name("off5"), of32 = rw.Name("off32"), dy = rw.Name("dy"), dx = rw.Name("dx");
        rw.Add("Reshape", {off->out[0], rw.ConstI("s5", {1, K, 2, H, W})}, {o5});
        rw.Add("Cast", {o5}, {of32}, {AInt("to", kFloat)});
        rw.Add("Split", {of32, rw.ConstI("sp", {1, 1})}, {dy, dx}, {AInt("axis", 2)});
        const std::string ax2 = rw.ConstI("ax2", {2}), dy4 = rw.Name("dy4"), dx4 = rw.Name("dx4");
        rw.Add("Squeeze", {dy, ax2}, {dy4});
        rw.Add("Squeeze", {dx, ax2}, {dx4});
        const std::string gx0 = rw.Name("gx0"), gx1 = rw.Name("gx1"), gx2 = rw.Name("gx2");
        const std::string gy0 = rw.Name("gy0"), gy1 = rw.Name("gy1"), gy2 = rw.Name("gy2");
        rw.Add("Mul", {dx4, rw.ConstF("sx", {}, {sx})}, {gx0});
        rw.Add("Add", {gx0, rw.ConstF("cwx", {1, 1, 1, W}, cwx)}, {gx1});
        rw.Add("Add", {gx1, rw.ConstF("ckx", {1, K, 1, 1}, ckx)}, {gx2});
        rw.Add("Mul", {dy4, rw.ConstF("sy", {}, {sy})}, {gy0});
        rw.Add("Add", {gy0, rw.ConstF("chy", {1, 1, H, 1}, chy)}, {gy1});
        rw.Add("Add", {gy1, rw.ConstF("cky", {1, K, 1, 1}, cky)}, {gy2});
        const std::string ax4 = rw.ConstI("ax4", {4}), ux = rw.Name("ux"), uy = rw.Name("uy");
        rw.Add("Unsqueeze", {gx2, ax4}, {ux});
        rw.Add("Unsqueeze", {gy2, ax4}, {uy});
        const std::string grid5 = rw.Name("grid5");
        std::string grid = rw.Name("grid");
        rw.Add("Concat", {ux, uy}, {grid5}, {AInt("axis", 4)});
        rw.Add("Reshape", {grid5, rw.ConstI("sg", {1, K * H, W, 2})}, {grid});
        if (half) {
            const std::string g16 = rw.Name("grid16");
            rw.Add("Cast", {grid}, {g16}, {AInt("to", kFloat16)});
            grid = g16;
        }
        const std::string samp = rw.Name("samp"), mk = rw.Name("mask"), sm = rw.Name("sampm"), col = rw.Name("col");
        rw.Add("GridSample", {off->in[0], grid}, {samp},
               {AStr("mode", "bilinear"), AStr("padding_mode", "zeros"), AInt("align_corners", 1)});
        rw.Add("Reshape", {mmul->out[0], rw.ConstI("sm", {1, 1, K * H, W})}, {mk});
        rw.Add("Mul", {samp, mk}, {sm});
        rw.Add("Reshape", {sm, rw.ConstI("sc", {1, C * K, H, W})}, {col});
        // the same weights seen as a 1x1 convolution over C*K channels: same bytes, new shape
        Init w2;
        w2.name = rw.Name("w");
        w2.built = TensorBytes(w2.name, w.type, {Cout, C * K, 1, 1}, w.data, w.dataLen);
        rw.newInits.push_back(std::move(w2));
        std::vector<std::string> cin = {col, rw.newInits.back().name};
        for (size_t i = 2; i < reg->in.size(); ++i) cin.push_back(reg->in[i]);
        rw.Add("Conv", cin, reg->out, {AInts("kernel_shape", {1, 1})});
        replace[ridx] = {first, rw.newNodes.size()};
    }

    // new node order, then drop whatever no longer feeds the output
    std::vector<const Node*> order;
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto r = replace.find(i);
        if (r != replace.end()) {
            for (size_t k = r->second.first; k < r->second.second; ++k) order.push_back(&rw.newNodes[k]);
        } else if (!remove.count(i)) {
            order.push_back(&nodes[i]);
        }
    }
    std::unordered_set<std::string> need(outputs.begin(), outputs.end());
    std::vector<bool> alive(order.size());
    for (size_t k = order.size(); k-- > 0;) {
        bool used = false;
        for (const std::string& o : order[k]->out) used = used || need.count(o);
        if (!used) continue;
        alive[k] = true;
        for (const std::string& in : order[k]->in)
            if (!in.empty()) need.insert(in);
    }

    Buf g;
    for (const Field& x : graph)
        if (x.num != kGraphNode && x.num != kGraphInit && x.num != kGraphValueInfo) g.Raw(x.raw, x.rawLen);
    for (size_t k = 0; k < order.size(); ++k) {
        if (!alive[k]) continue;
        const Node& n = *order[k];
        if (n.raw) g.Bytes(kGraphNode, n.raw, n.rawLen);
        else g.Str(kGraphNode, n.built);
    }
    for (const Init& t : inits)
        if (need.count(t.name)) g.Bytes(kGraphInit, t.raw, t.rawLen);
    for (const Init& t : rw.newInits)
        if (need.count(t.name)) g.Str(kGraphInit, t.built);

    Buf head;
    for (const Field& x : model)
        if (&x != gf) head.Raw(x.raw, x.rawLen);
    head.Tag(kModelGraph, 2);
    head.Varint(g.s.size());

    HANDLE o = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (o == INVALID_HANDLE_VALUE) {
        err = L"Couldn't write the AI model (disk full?).";
        return false;
    }
    const bool ok = WriteAll(o, head.s.data(), head.s.size()) && WriteAll(o, g.s.data(), g.s.size());
    CloseHandle(o);
    if (!ok) {
        DeleteFileW(dst.c_str());
        err = L"Couldn't write the AI model (disk full?).";
        return false;
    }
    Log(L"rmbg: model rewritten, %d deformable convolutions replaced", static_cast<int>(scopes.size()));
    err.clear();
    return true;
}
