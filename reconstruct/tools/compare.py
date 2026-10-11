"""Reconstruction dev comparisons.

  compare.py points <a.json> <ref.json>   per-LED distance after similarity alignment of a onto ref
  compare.py obs <a_dir> <ref_dir>        per-video diff of decoder observation JSONs (a vs ref)
"""
import json
import os
import sys

import numpy as np


def load_points(path):
    leds = json.load(open(path))["leds"]
    pts = np.array([[np.nan if v is None else v for v in p] for p in leds], dtype=float)
    return pts, ~np.isnan(pts).any(axis=1)


def umeyama(src, dst):
    mu_s, mu_d = src.mean(0), dst.mean(0)
    s, d = src - mu_s, dst - mu_d
    u, sig, vt = np.linalg.svd(d.T @ s / len(src))
    e = np.ones(3)
    if np.linalg.det(u) * np.linalg.det(vt) < 0:
        e[2] = -1
    r = u @ np.diag(e) @ vt
    scale = (sig * e).sum() / (s ** 2).sum(axis=1).mean()
    return scale, r, mu_d - scale * r @ mu_s


def cmd_points(a_path, ref_path):
    a, a_ok = load_points(a_path)
    ref, ref_ok = load_points(ref_path)
    n = min(len(a), len(ref))
    common = np.flatnonzero(a_ok[:n] & ref_ok[:n])
    if len(common) < 3:
        sys.exit("fewer than 3 common LEDs")

    # align on all common LEDs, then once more without gross outliers
    inl = common
    for _ in range(2):
        scale, r, t = umeyama(a[inl], ref[inl])
        dist = np.linalg.norm((scale * (r @ a[common].T)).T + t - ref[common], axis=1)
        inl = common[dist < max(5 * np.median(dist), 1e-9)]

    height = np.ptp(ref[ref_ok], axis=0).max()
    print(f"valid: a={a_ok.sum()} ref={ref_ok.sum()} common={len(common)} only_a={(a_ok[:n] & ~ref_ok[:n]).sum()} only_ref={(~a_ok[:n] & ref_ok[:n]).sum()}")
    print(f"alignment: scale={scale:.6f} (fit on {len(inl)})")
    print(f"distance (ref units; ref extent {height:.4f}):")
    for name, v in (("median", np.median(dist)), ("mean", dist.mean()), ("p90", np.percentile(dist, 90)),
                    ("p99", np.percentile(dist, 99)), ("max", dist.max())):
        print(f"  {name:6s} {v:.4g}  ({100 * v / height:.4g}% of extent)")
    for frac in (0.01, 0.02, 0.05):
        print(f"  > {100 * frac:.0f}% of extent: {(dist > frac * height).sum()}")


def load_obs(path):
    # {led: [[row, col], area]}
    return {int(k): np.array(v[0], dtype=float) for k, v in json.load(open(path)).items()}


def cmd_obs(a_dir, ref_dir, match_px=3.0):
    names = sorted(n for n in os.listdir(ref_dir) if n.endswith(".json"))
    a_names = set(n for n in os.listdir(a_dir) if n.endswith(".json"))
    tot = {"ref": 0, "a": 0, "match": 0, "moved": 0, "only_a": 0, "only_ref": 0}
    errs = []
    missing = [n for n in names if n not in a_names]
    extra = sorted(a_names - set(names))
    print(f"{'video':28s} {'ref':>6s} {'a':>6s} {'match':>6s} {'moved':>6s} {'only_a':>6s} {'only_ref':>8s} {'med_px':>7s} {'max_px':>7s}")
    for n in names:
        if n not in a_names:
            continue
        a, ref = load_obs(os.path.join(a_dir, n)), load_obs(os.path.join(ref_dir, n))
        both = sorted(set(a) & set(ref))
        d = np.array([np.linalg.norm(a[k] - ref[k]) for k in both])
        row = {"ref": len(ref), "a": len(a), "match": int((d <= match_px).sum()), "moved": int((d > match_px).sum()),
               "only_a": len(set(a) - set(ref)), "only_ref": len(set(ref) - set(a))}
        for k in tot:
            tot[k] += row[k]
        errs.append(d[d <= match_px])
        med = f"{np.median(d):.3f}" if len(d) else "-"
        mx = f"{d.max():.3f}" if len(d) else "-"
        print(f"{n:28s} {row['ref']:6d} {row['a']:6d} {row['match']:6d} {row['moved']:6d} {row['only_a']:6d} {row['only_ref']:8d} {med:>7s} {mx:>7s}")

    e = np.concatenate(errs) if errs else np.zeros(0)
    print(f"\ntotal: {tot}")
    if len(e):
        print(f"matched centroid error px: median={np.median(e):.4f} p95={np.percentile(e, 95):.4f} max={e.max():.4f}")
    print(f"videos only in ref: {len(missing)} {missing}")
    print(f"videos only in a: {len(extra)} {extra}")


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("points", "obs"):
        sys.exit(__doc__)
    {"points": cmd_points, "obs": cmd_obs}[sys.argv[1]](sys.argv[2], sys.argv[3])


if __name__ == "__main__":
    main()
