if [ -z "$RES" ]; then
  RES=80x60
fi
temp=$(mktemp --suffix=".mp4")
ffmpeg -y -i "$1" -r 5 -s "$RES" -an "$temp"
./a.out -i "$temp" -o "out.vid" -c 12
rm "$temp"
