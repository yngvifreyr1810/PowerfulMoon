"""The release's pipeline seed from a recorded cache: make_pipeline_seed.py <cache/graphics-pipelines.bin>... <out>

cache/graphics-pipelines.bin holds the pipeline states a PC's runs recorded (runtime/nvn/vulkan/exec.c
vk_pipeline_recipes_save): a header (version, recipe size, count, reserved, the GPU's pipeline cache UUID, a checksum
of the recipes) and the recipes - hashes of translated shaders and fixed-function state, nothing of the game itself.
The seed is their union (in the order first seen) with an all-zero UUID: exec.c load_pipeline_recipes keeps the
recipes another GPU supports, and the setup's shader step compiles them ahead (data/pipeline-seed.bin.zst, packed).

The recipes name translated shaders by key, so a change to the shader translator (runtime/nvn/maxwell/translate.c and
friends) leaves old recipes pointing at nothing - 0.5.0 shipped a seed recorded with 0.1.0's translator, 63% of it
unusable, and a new player's opening built ~700 pipelines while playing. tools/release.sh step 3b stops a release when
more than 5% are left out. To regenerate (2026-11-19, 8,315 recipes: 1 pipeline built in the opening instead of 828):
  1. Empty data roots linking the test copies' game files (mklink /J <root>\extracted <test copy>\extracted), one per
     game version, a build of the release's code.
  2. In each, record: a new game's opening into Cap Kingdom (A every 150 frames), then every kingdom by warp with
     walking, the camera, the pause menu and the map (ODYSSEY_PREPARE=0; the scripts were seedrun.sh in that session).
     Each root's cache/graphics-pipelines.bin then holds what was used.
  3. The old seed's recipes that still build: in one root, the old seed as cache/graphics-pipelines.bin and
     `SuperOdysseyRecomp --prepare-shaders all`, then `... recorded` with ODYSSEY_PREP_KEPT_OUT=<file>.
  4. make_pipeline_seed.py <root1 recipes> <root2 recipes> <kept file> <seed.bin>, then
     zstd -19 <seed.bin> -o data/pipeline-seed.bin.zst (the repository takes no file over 5 MB; the setup unpacks it).
  Remove the links with rmdir (never delete through them: that deletes the test copy's game files).
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
