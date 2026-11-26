#!/bin/bash
# Encode the screencast frames from record-demo.py into an MP4 (run after the QNX workload has stopped).
set -e
D=${1:-demo}
cd "$D/frames"
mv ../frames.ffconcat .
ffmpeg -loglevel error -y -f concat -safe 0 -i frames.ffconcat -vf "fps=30,format=yuv420p" \
  -c:v libx264 -preset veryfast -crf 20 -movflags +faststart ../demo.mp4
cd .. && rm -rf frames && ls -la
