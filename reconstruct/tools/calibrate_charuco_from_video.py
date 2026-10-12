"""Calibrate the reconstruction camera from videos of the ChArUco target.

  calibrate_charuco_from_video.py <video> [<video> ...] [--output calib.json] [--every N]

Film make_charuco_target.py's board from many angles and distances, filling the frame corners too,
with the same camera, zoom and resolution as the tree capture.
Writes the solver's --calibration JSON: image size, pinhole intrinsics, OpenCV 5-coef distortion.
"""
import argparse
import json

import cv2

from make_charuco_target import make_board

MIN_CORNERS = 6


def detect_views(path, detector, board, every):
    obj_points, img_points = [], []
    size = None
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        raise SystemExit(f"Can't open {path}")
    frame_idx = 0
    while True:
        ok, img = cap.read()
        if not ok:
            break
        size = (img.shape[1], img.shape[0])
        frame_idx += 1
        if (frame_idx - 1) % every:
            continue
        corners, ids, _, _ = detector.detectBoard(cv2.cvtColor(img, cv2.COLOR_BGR2GRAY))
        if ids is None or len(ids) < MIN_CORNERS or board.checkCharucoCornersCollinear(ids):
            continue
        obj, imgp = board.matchImagePoints(corners, ids)
        obj_points.append(obj)
        img_points.append(imgp)
    print(f"{path}: {frame_idx} frames, {len(obj_points)} usable views")
    return obj_points, img_points, size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("videos", nargs="+")
    ap.add_argument("--output", default="calib.json")
    ap.add_argument("--every", type=int, default=20, help="use every Nth frame")
    args = ap.parse_args()

    board = make_board()
    detector = cv2.aruco.CharucoDetector(board)
    obj_points, img_points = [], []
    size = None
    for path in args.videos:
        obj, imgp, video_size = detect_views(path, detector, board, args.every)
        if size and video_size != size:
            raise SystemExit(f"{path} is {video_size}, expected {size}")
        size = video_size
        obj_points += obj
        img_points += imgp
    if len(obj_points) < 10:
        raise SystemExit(f"Only {len(obj_points)} usable views; need at least 10")

    rms, k, dist, _, _ = cv2.calibrateCamera(obj_points, img_points, size, None, None)
    print(f"{len(obj_points)} views, {size[0]}x{size[1]}, RMS reprojection error {rms:.3f} px")

    calib = {
        "width": size[0],
        "height": size[1],
        "fx": k[0, 0],
        "fy": k[1, 1],
        "cx": k[0, 2],
        "cy": k[1, 2],
        "dist": dist.ravel()[:5].tolist(),  # k1 k2 p1 p2 k3
    }
    with open(args.output, "w") as f:
        json.dump(calib, f, indent=2)
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
