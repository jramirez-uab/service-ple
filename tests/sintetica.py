"""Synthetic PLE table with the real layout (shards in alphabetical order, short last shard, bf16 scale) for the tests.
usage: python3 sintetica.py OUT.safetensors"""
import json, struct, sys
F, R, B, ULTIMO = 12, 1000, 160, 377
pre = "model.language_model.layers.1.ple.ple_embedding.ngram_embedding."
h = {"__metadata__": {"format": "pt"}, pre + "weight_scale": {"dtype": "BF16", "shape": [1], "data_offsets": [0, 2]}}
blobs, off = [b"\x3c\x80"], 2
for n in sorted(f"shard_{i}" for i in range(F)):
    i = int(n.split("_")[1]); r = ULTIMO if i == F - 1 else R
    data = bytes(((i * 131 + k * 7 + (k // B) * 13) & 255) for k in range(r * B))
    h[pre + n + ".weight"] = {"dtype": "F8_E4M3", "shape": [r, B], "data_offsets": [off, off + len(data)]}
    blobs.append(data); off += len(data)
hj = json.dumps(h).encode()
with open(sys.argv[1], "wb") as f:
    f.write(struct.pack("<Q", len(hj))); f.write(hj); [f.write(b) for b in blobs]
print("rows", R * (F - 1) + ULTIMO)
