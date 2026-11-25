"""Reference IMA ADPCM block codec for the phone link (docs/phone-link-v2.md, section 3).
Writes docs/adpcm-test-vector.json: a 440 Hz sine, 256 samples at 16 kHz, peak 8000."""
import json, math, os

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
clamp = lambda v, lo, hi: max(lo, min(hi, v))

def step_once(pred, idx, code):
    """Apply one 4-bit code to the decoder state; returns the new (pred, idx)."""
    s = STEP[idx]
    diff = s >> 3
    if code & 4: diff += s
    if code & 2: diff += s >> 1
    if code & 1: diff += s >> 2
    pred = pred - diff if code & 8 else pred + diff
    return clamp(pred, -32768, 32767), clamp(idx + INDEX[code], 0, 88)

def encode_block(samples, pred, idx):
    """samples: list of int16. Header predictor/index = the state BEFORE the first sample."""
    head = bytes([0x01]) + int(pred).to_bytes(2, 'little', signed=True) + bytes([idx, len(samples) & 0xFF])
    codes = []
    for x in samples:
        s = STEP[idx]
        d = x - pred
        code = 0
        if d < 0: code = 8; d = -d
        if d >= s: code |= 4; d -= s
        if d >= s >> 1: code |= 2; d -= s >> 1
        if d >= s >> 2: code |= 1
        pred, idx = step_once(pred, idx, code)
        codes.append(code)
    if len(codes) % 2: codes.append(0)
    body = bytes(codes[i] | (codes[i + 1] << 4) for i in range(0, len(codes), 2))   # low nibble first
    return head + body, pred, idx

def decode_block(b):
    assert b[0] == 0x01
    pred = int.from_bytes(b[1:3], 'little', signed=True); idx = b[3]; n = b[4] or 256
    out = []
    for i in range(n):
        byte = b[5 + i // 2]
        code = byte & 0x0F if i % 2 == 0 else byte >> 4
        pred, idx = step_once(pred, idx, code)
        out.append(pred)
    return out

if __name__ == '__main__':
    pcm = [int(round(8000 * math.sin(2 * math.pi * 440 * i / 16000))) for i in range(256)]
    block, _, _ = encode_block(pcm, 0, 0)
    dec = decode_block(block)
    err = max(abs(a - b) for a, b in zip(pcm, dec))
    out = {"description": "IMA ADPCM phone-link block (docs/phone-link-v2.md s3): 440 Hz sine, 16 kHz, peak 8000, initial state pred 0 index 0",
           "pcm": pcm, "block_hex": block.hex(), "decoded": dec, "max_abs_error": err}
    p = os.path.join(os.path.dirname(__file__), '..', 'docs', 'adpcm-test-vector.json')
    json.dump(out, open(p, 'w'), indent=1)
    print("block", len(block), "bytes, max error", err)
