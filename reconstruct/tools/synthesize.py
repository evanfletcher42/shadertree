"""Render synthetic reconstruction videos from known LED positions.

  synthesize.py <points.json> <calib.json> <out_dir> [--views N] [--seed S]

Writes <out_dir>/synth_NNN.MOV (ledcodes.glsl blink sequence seen from a ring of cameras) and
<out_dir>/truth/synth_NNN.MOV.json (true projections of visible LEDs, in decoder output format).
Points are assumed aligned with +z up (solver aligned output).
"""
import argparse
import json
import os

import cv2
import numpy as np

FPS = 59.94  # ledcodes.glsl
WIDTH, HEIGHT = 1920, 1080
NUM_FRAMES = 3 * 70  # three 35-slot cycles at 2 frames/slot
FILL = (0.9, 1.8)  # tree height as a fraction of image width (real footage is rolled ~90deg, tip right)
ROLL_DEG = (85, 95)
EXPOSURE = 0.5  # fraction of a frame the shutter is open
BLOB_SIGMA = 2.5
GAIN = 12000.0  # saturates bright blob cores like real footage
BACKGROUND = 4.0
NOISE_SIGMA = 3.0
DROPOUT = 0.05
BACK_OCCLUSION = -0.3  # hide LEDs whose radial direction faces away from the camera beyond this cosine
CROSSTALK = np.array([[1.0, 0.12, 0.03],
                      [0.10, 1.0, 0.15],
                      [0.04, 0.20, 1.0]])  # rows: R, G, B emitter -> camera RGB

HADAMARD = np.array([
    [0, 0, 0, 0, 0, 0, 0, 0], [0, 1, 0, 1, 0, 1, 0, 1], [0, 0, 1, 1, 0, 0, 1, 1], [0, 1, 1, 0, 0, 1, 1, 0],
    [0, 0, 0, 0, 1, 1, 1, 1], [0, 1, 0, 1, 1, 0, 1, 0], [0, 0, 1, 1, 1, 1, 0, 0], [0, 1, 1, 0, 1, 0, 0, 1],
    [1, 1, 1, 1, 1, 1, 1, 1], [1, 0, 1, 0, 1, 0, 1, 0], [1, 1, 0, 0, 1, 1, 0, 0], [1, 0, 0, 1, 1, 0, 0, 1],
    [1, 1, 1, 1, 0, 0, 0, 0], [1, 0, 1, 0, 0, 1, 0, 1], [1, 1, 0, 0, 0, 0, 1, 1], [1, 0, 0, 1, 0, 1, 1, 0],
]) * 2 - 1


def slot_colors(idx):
    """Color per slot (35) per LED: 0..2 = R/G/B, 3 = white. Mirrors ledcodes.glsl."""
    bits = np.concatenate([HADAMARD[(idx >> s) & 0xF] for s in (12, 8, 4, 0)], axis=1)  # (n, 32) of +-1
    ofs = idx % 3
    colors = np.empty((35, len(idx)), dtype=int)
    colors[0:2] = 3
    colors[2] = ofs % 3
    colors[3:] = ((ofs[:, None] + np.cumsum(bits, axis=1)) % 3).T
    return colors


def look_at(eye, target, roll):
    fwd = target - eye
    fwd /= np.linalg.norm(fwd)
    right = np.cross(fwd, [0, 0, 1.0])
    right /= np.linalg.norm(right)
    down = np.cross(fwd, right)
    c, s = np.cos(roll), np.sin(roll)
    r = np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]]) @ np.stack([right, down, fwd])  # camera-from-world, OpenCV axes
    return cv2.Rodrigues(r)[0], -r @ eye


def splat(img, rc, rgb):
    """Bilinear splat of point intensities, preserving the subpixel centroid."""
    r0, c0 = np.floor(rc[:, 0]).astype(int), np.floor(rc[:, 1]).astype(int)
    fr, fc = rc[:, 0] - r0, rc[:, 1] - c0
    for dr, dc, w in ((0, 0, (1 - fr) * (1 - fc)), (0, 1, (1 - fr) * fc), (1, 0, fr * (1 - fc)), (1, 1, fr * fc)):
        rr, cc = r0 + dr, c0 + dc
        ok = (rr >= 0) & (rr < HEIGHT) & (cc >= 0) & (cc < WIDTH)
        np.add.at(img, (rr[ok], cc[ok]), rgb[ok] * w[ok, None])


def render_view(path, pts, idx, colors, rvec, tvec, k, dist, axis_xy, rng):
    proj = cv2.projectPoints(pts, rvec, tvec, k, dist)[0][:, 0, :]  # (n, 2) x,y
    in_cam = (cv2.Rodrigues(rvec)[0] @ pts.T).T + tvec
    eye = -cv2.Rodrigues(rvec)[0].T @ tvec

    radial = pts[:, :2] - axis_xy
    radial /= np.linalg.norm(radial, axis=1, keepdims=True) + 1e-9
    to_cam = eye[:2] - axis_xy
    to_cam /= np.linalg.norm(to_cam)
    visible = ((in_cam[:, 2] > 0) & (radial @ to_cam > BACK_OCCLUSION) & (rng.random(len(pts)) > DROPOUT) &
               (proj[:, 0] >= 2) & (proj[:, 0] < WIDTH - 2) & (proj[:, 1] >= 2) & (proj[:, 1] < HEIGHT - 2))

    rc = proj[visible][:, ::-1].copy()
    cols = colors[:, visible]
    bright = rng.uniform(0.4, 1.0, visible.sum())[:, None]
    palette = np.vstack([CROSSTALK, np.ones((1, 3)) * 1.3])

    writer = cv2.VideoWriter(path, cv2.VideoWriter_fourcc(*"mp4v"), FPS, (WIDTH, HEIGHT))
    if not writer.isOpened():
        raise RuntimeError("VideoWriter failed: " + path)

    t0 = rng.uniform(0, 70)  # tree frame at camera frame 0
    for f in range(NUM_FRAMES):
        img = np.full((HEIGHT, WIDTH, 3), BACKGROUND, dtype=np.float32)
        # integrate the shutter over two samples so slot boundaries blend like real footage
        for s in (0.0, EXPOSURE):
            slot = (int(t0 + f + s) // 2) % 35
            splat(img, rc, palette[cols[slot]] * bright * (GAIN / 2))
        img = cv2.GaussianBlur(img, (0, 0), BLOB_SIGMA)
        img += rng.normal(0, NOISE_SIGMA, img.shape).astype(np.float32)
        writer.write(np.clip(img[:, :, ::-1], 0, 255).astype(np.uint8))  # BGR
    writer.release()

    return {int(i): [[float(r), float(c)], 0.0] for i, (r, c) in zip(idx[visible], rc)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("points")
    ap.add_argument("calib")
    ap.add_argument("out_dir")
    ap.add_argument("--views", type=int, default=24)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    leds = json.load(open(args.points))["leds"]
    idx = np.array([i for i, p in enumerate(leds) if p[0] is not None])
    pts = np.array([leds[i] for i in idx], dtype=float)
    cal = json.load(open(args.calib))
    k = np.array([[cal["fx"], 0, cal["cx"]], [0, cal["fy"], cal["cy"]], [0, 0, 1]])
    dist = np.array(cal["dist"], dtype=float)

    rng = np.random.default_rng(args.seed)
    colors = slot_colors(idx)
    center = pts.mean(0)
    lo, hi = pts[:, 2].min(), pts[:, 2].max()
    height = hi - lo
    max_radial = np.linalg.norm(pts[:, :2] - center[:2], axis=1).max()

    os.makedirs(os.path.join(args.out_dir, "truth"), exist_ok=True)
    for v in range(args.views):
        yaw = 2 * np.pi * (v + rng.uniform(-0.25, 0.25)) / args.views
        radius = max(height * cal["fx"] / (rng.uniform(*FILL) * WIDTH), 1.5 * max_radial)
        target = np.array([center[0], center[1], lo + rng.uniform(0.3, 0.7) * height])
        eye = target + [radius * np.cos(yaw), radius * np.sin(yaw), rng.uniform(-0.1, 0.4) * height]
        rvec, tvec = look_at(eye, target + rng.normal(0, 0.03 * height, 3), np.radians(rng.uniform(*ROLL_DEG)))
        name = f"synth_{v:03d}.MOV"
        truth = render_view(os.path.join(args.out_dir, name), pts, idx, colors, rvec, tvec, k, dist, center[:2], rng)
        json.dump(truth, open(os.path.join(args.out_dir, "truth", name + ".json"), "w"))
        print(f"{name}: {len(truth)} visible LEDs")


if __name__ == "__main__":
    main()
