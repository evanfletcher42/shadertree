"""Render the ChArUco calibration target.

  make_charuco_target.py [out.png]

Show it full screen on a 16:9 monitor (or print it) and film it with calibrate_charuco_from_video.py.
"""
import sys

import cv2


def make_board():
    aruco_dict = cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_6X6_250)
    return cv2.aruco.CharucoBoard((16, 9), 1.0, 0.5, aruco_dict)


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "charuco.png"
    cv2.imwrite(out_path, make_board().generateImage((1920, 1080)))


if __name__ == "__main__":
    main()
