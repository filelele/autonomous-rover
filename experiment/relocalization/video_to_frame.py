import numpy as np
import cv2 as cv
import os
import glob
import sys

def video_to_frame(video_path, output_dir, frame_rate=1, blur_threshold=100):
    """
    Extract frames from a video file and save them as images.

    Parameters:
    video_path (str): Path to the input video file.
    output_dir (str): Directory where the extracted frames will be saved.
    frame_rate (int): Number of frames to skip between each saved frame. Default is 1 (save every frame).
    blur_threshold (int): Threshold for determining if a frame is blurry. Default is 100.
    """
    
    # Create output directory if it doesn't exist
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)

    # Open the video file
    cap = cv.VideoCapture(video_path)
    
    if not cap.isOpened():
        print(f"Error: Could not open video {video_path}")
        return

    frame_count = 0
    saved_frame_count = 0

    while True:
        ret, frame = cap.read()

        gray = cv.cvtColor(frame, cv.COLOR_BGR2GRAY)

        # Calculate gradients along X and Y axes
        sobelx = cv.Sobel(gray, cv.CV_64F, 1, 0, ksize=3)
        sobely = cv.Sobel(gray, cv.CV_64F, 0, 1, ksize=3)

        # Calculate Tenengrad focus measure (sum of squared gradients)
        tenengrad_score = (sobelx ** 2 + sobely ** 2).mean()

        if tenengrad_score < blur_threshold:
            continue

        if not ret:
            break
        
        # Save every 'frame_rate' frames
        if frame_count % frame_rate == 0:
            frame_filename = os.path.join(output_dir, f"frame_{saved_frame_count:04d}.jpg")
            cv.imwrite(frame_filename, frame)
            saved_frame_count += 1
        
        frame_count += 1

    cap.release()
    print(f"Extracted {saved_frame_count} frames from {video_path} to {output_dir}")

def main():
    if sys.argv[1] == "--help" or len(sys.argv) < 3:
        print("Usage: python video_to_frame.py <video_path> <output_dir> [frame_rate] [blur_threshold]")
        print("Example: python video_to_frame.py input_video.mp4 output_frames 5 100")
        sys.exit(1)

    video_path = sys.argv[1]
    output_dir = sys.argv[2]
    frame_rate = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    blur_threshold = int(sys.argv[4]) if len(sys.argv) > 4 else 100

    video_to_frame(video_path, output_dir, frame_rate, blur_threshold)

if __name__ == "__main__":
    main()