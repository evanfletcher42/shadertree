import numpy as np
import matplotlib.pyplot as plt
from matplotlib.colors import ListedColormap, BoundaryNorm
import cv2
import scipy
import pprint
import glob
import json
import os
import sys

# Read video
def read_video(video_path):
    frames = []
    cap = cv2.VideoCapture(video_path)

    while True:
        ret, frame = cap.read()

        if not ret:
            cap.release()
            return frames

        frame_rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        frames.append(frame_rgb)


def svm_extract_rgb(X):
    # Normalize to [0, 1] assuming 0–255 input
    x = X / 255.0

    W = np.array([
        [1.0, -0.5, -0.5],  # red
        [-0.5, 1.0, -0.5],  # green
        [-0.5, -0.5, 1.0],  # blue
        # [1.0, 1.0, 1.0],  # white
        # [-1.0, -1.0, -1.0],  # black
    ])

    b = np.array([
        0.0,  # red
        0.0,  # green
        0.0,  # blue
        #-2.1,  # white
        #0.3,  # black
    ])

    scores = x @ W.T + b
    idx = np.argmax(scores, axis=-1)

    return idx


def extract_sync_centers(data):
    intensity = np.mean(data, axis=(1, 2, 3))

    conv_w = 5
    intensity_mavg = np.convolve(intensity, np.ones(conv_w), mode='same')

    peaks, meta = scipy.signal.find_peaks(intensity_mavg, prominence=30, distance=40)

    # trim peaks that are too close to an edge
    peaks = peaks[ peaks > conv_w ]
    peaks = peaks[ peaks < len(intensity_mavg) - conv_w]

    # plt.plot(intensity)
    # for p in peaks:
    #     plt.axvline(p, color='black')
    #
    # plt.show()

    return peaks

def viz_frame_colors(f):
    colors = ['red', 'green', 'blue']#, 'white', 'black']
    cmap = ListedColormap(colors)

    bounds = [-0.5, 0.5, 1.5, 2.5]#, 3.5, 4.5]
    norm = BoundaryNorm(bounds, cmap.N)
    plt.figure()
    plt.imshow(f, cmap=cmap, norm=norm)

def decode_hadamard(x):
    CODEBOOK = np.array([
        [0, 0, 0, 0, 0, 0, 0, 0],
        [0, 1, 0, 1, 0, 1, 0, 1],
        [0, 0, 1, 1, 0, 0, 1, 1],
        [0, 1, 1, 0, 0, 1, 1, 0],
        [0, 0, 0, 0, 1, 1, 1, 1],
        [0, 1, 0, 1, 1, 0, 1, 0],
        [0, 0, 1, 1, 1, 1, 0, 0],
        [0, 1, 1, 0, 1, 0, 0, 1],
        [1, 1, 1, 1, 1, 1, 1, 1],
        [1, 0, 1, 0, 1, 0, 1, 0],
        [1, 1, 0, 0, 1, 1, 0, 0],
        [1, 0, 0, 1, 1, 0, 0, 1],
        [1, 1, 1, 1, 0, 0, 0, 0],
        [1, 0, 1, 0, 0, 1, 0, 1],
        [1, 1, 0, 0, 0, 0, 1, 1],
        [1, 0, 0, 1, 0, 1, 1, 0],
    ], dtype=np.int8)

    # Move codeword bits to the last axis: (a, b, 8)
    cw = np.moveaxis(x, 0, -1)

    # Known bits (not erased)
    known = (cw != 2)  # (a, b, 8)
    known_count = known.sum(axis=-1)  # (a, b)

    # Compare against all 16 codewords
    # cw[..., None, :] -> (a, b, 1, 8)
    # CODEBOOK         -> (1, 1, 16, 8)
    # result           -> (a, b, 16, 8)
    mismatches = (cw[..., None, :] != CODEBOOK) & known[..., None, :]

    # Hamming distance over **known** positions
    dist = mismatches.sum(axis=-1)  # (a, b, 16)

    # Best codeword per position
    best_dist = dist.min(axis=-1)  # (a, b)
    decoded = dist.argmin(axis=-1)  # (a, b), values 0..15

    # Number of matching known bits = total known - mismatches
    matches = known_count - best_dist  # (a, b)

    # Successful decode if we have enough correct matches
    # success_mask = (known_count >= 5) & (matches >= 5)

    # Successful decode iff we saw the whole sequence and it had no errors
    # we _really_ do not want to speculatively reconstruct something that's busted
    success_mask = (known_count == 8) & (matches == known_count)

    return decoded, success_mask


def find_all_blobs(arr: np.ndarray, background=None):
    """
    arr: 2D numpy array (rows, cols), dtype int
    background: value to ignore (e.g. 0). If None, include all values.

    Returns:
        dict: { value: ((row_center, col_center), size) }
        - center is returned as (row, col) in float
        - size is the contour area in pixels (float)
    """
    assert arr.ndim == 2, "Expected a 2D array"

    result = {}

    for val in np.unique(arr):
        if background is not None and val == background:
            continue

        # Binary mask for this value
        mask = (arr == val).astype(np.uint8)

        contours, _ = cv2.findContours(
            mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE
        )

        best_area = 0.0
        best_center = None

        for c in contours:
            area = cv2.contourArea(c)
            if area <= 3:
                continue

            M = cv2.moments(c)
            if M["m00"] == 0:
                continue

            # OpenCV gives (cx, cy) = (col, row)
            cx = M["m10"] / M["m00"]
            cy = M["m01"] / M["m00"]

            if area > best_area:
                best_area = area
                best_center = (cy, cx)  # return as (row, col)

        if best_center is not None:
            result[int(val)] = (best_center, best_area)

    return result


def process_video(video_path):
    print("Processing", video_path)

    if os.path.exists(video_path + ".json"):
        print("Output already done, skip")
        return

    data = read_video(video_path)
    data = np.array(data)

    sync_centers = extract_sync_centers(data)
    print("sync_centers", sync_centers)

    # generate the indices of the 35 flash centers and subsample to those
    slot_width = (sync_centers[1] - sync_centers[0]) / 35.0
    print("slot_width", slot_width)
    slot_centers = sync_centers[0] + slot_width * (0.5 + np.arange(33) + 1)
    slot_centers = (slot_centers + 0.5).astype(int)
    print("slot_centers")
    print(slot_centers)

    data = data[slot_centers]

    print("data", data.shape)

    # perform color classification
    idxs = svm_extract_rgb(data)
    print("idxs", idxs.shape)

    # any pixel that is ever classified as 'black' (idx==4) should be masked out on all frames
    #mask = (idxs == 4).any(axis=0)
    #idxs[:, mask] = 4

    # also wipe any "white" pixels (set them to black)
    #mask = (idxs == 3).any(axis=0)
    #idxs[:, mask] = 4

    # Create per pixel bitmask (compare edges)
    bits = np.zeros((32, idxs.shape[1], idxs.shape[2]), dtype=int)
    bits[...] = 2  # 2 = "erasure"

    for i in range(idxs.shape[0] - 1):
        mask_one = (
                ((idxs[i] == 0) & (idxs[i + 1] == 1)) |
                ((idxs[i] == 1) & (idxs[i + 1] == 2)) |
                ((idxs[i] == 2) & (idxs[i + 1] == 0))
        )

        mask_zero = (
                ((idxs[i] == 0) & (idxs[i + 1] == 2)) |
                ((idxs[i] == 2) & (idxs[i + 1] == 1)) |
                ((idxs[i] == 1) & (idxs[i + 1] == 0))
        )

        bits[i, mask_one] = 1
        bits[i, mask_zero] = 0

    decoded_a, success_mask_a = decode_hadamard(bits[0:8])
    decoded_b, success_mask_b = decode_hadamard(bits[8:16])
    decoded_c, success_mask_c = decode_hadamard(bits[16:24])
    decoded_d, success_mask_d = decode_hadamard(bits[24:32])

    # combine
    success_mask = success_mask_a & success_mask_b & success_mask_c & success_mask_d
    decoded = (decoded_a << 12) | (decoded_b << 8) | (decoded_c << 4) | decoded_d

    decoded[np.logical_not(success_mask)] = -1

    # plt.imshow(decoded, cmap='prism')
    # plt.show()

    blobs = find_all_blobs(decoded, background=-1)
    print("Found", len(blobs), "LEDs")

    # plt.figure()
    # plt.imshow(data[0])
    # for led_idx in blobs:
    #     ((r, c), sz) = blobs[led_idx]
    #     plt.scatter(c, r, marker='+')
    #     plt.text(c, r, str(led_idx), color='white')
    #
    # plt.show()

    with open(video_path + ".json", 'w') as jf:
        json.dump(blobs, jf, indent=4)


def main(tid):
    print("Start TID", tid)
    paths = glob.glob("../ledreconstruct/ledreconstruct/2025-static/*" + str(tid) + ".MOV")
    for vp in paths:
        try:
            process_video(vp)
        except Exception as e:
            print(e)



if __name__ == "__main__":
    main(int(sys.argv[1]))



# Find sync centers

# Per pixel RGB classification

# Per pixel RGB sequence

# Per pixel hadamard decode

