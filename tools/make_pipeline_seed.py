"""The release's pipeline seed from a recorded cache: make_pipeline_seed.py <cache/graphics-pipelines.bin>... <out>

cache/graphics-pipelines.bin holds the pipeline states a PC's runs recorded (runtime/nvn/vulkan/exec.c
vk_pipeline_recipes_save): a header (version, recipe size, count, reserved, the GPU's pipeline cache UUID, a checksum
of the recipes) and the recipes - hashes of translated shaders and fixed-function state, nothing of the game itself.
The seed is their union (in the order first seen) with an all-zero UUID: exec.c load_pipeline_recipes keeps the
recipes another GPU supports, and the setup's shader step compiles them ahead (data/pipeline-seed.bin).
"""
import struct
import sys

HEADER = struct.Struct('<4I16sQ')
MASK = (1 << 64) - 1


def hash_words(h, data):
    """exec.c hash_words: 64-bit little-endian words mixed in, then the tail bytes (FNV-1a), then folded."""
    n8 = len(data) // 8
    for (w,) in struct.iter_unpack('<Q', data[:n8 * 8]):
        h = ((h ^ w) * 0x9E3779B97F4A7C15) & MASK
        h ^= h >> 29
    for b in data[n8 * 8:]:
        h = ((h ^ b) * 0x100000001B3) & MASK
    return h ^ (h >> 32)


def read(path):
    raw = open(path, 'rb').read()
    version, size, count, _, uuid, checksum = HEADER.unpack_from(raw)
    assert version == 1 and len(raw) == HEADER.size + size * count, f'{path}: not a recipe file (v{version}, {size} x {count})'
    body = raw[HEADER.size:]
    return size, [body[i * size:(i + 1) * size] for i in range(count)], checksum, body


def main():
    *inputs, out = sys.argv[1:]
    size, seen, recipes = None, set(), []
    for p in inputs:
        s, rs, checksum, body = read(p)
        size = size or s
        assert s == size, f'{p}: recipe size {s}, others {size}'
        print(f'{p}: {len(rs)} recipes, checksum {"ok" if hash_words(0, body) == checksum else "DIFFERENT (hash_words?)"}')
        for r in rs:
            if r not in seen:
                seen.add(r)
                recipes.append(r)
    body = b''.join(recipes)
    with open(out, 'wb') as f:
        f.write(HEADER.pack(1, size, len(recipes), 0, bytes(16), hash_words(0, body)))
        f.write(body)
    print(f'{out}: {len(recipes)} recipes ({len(body) + HEADER.size:,} bytes)')


if __name__ == '__main__':
    main()
