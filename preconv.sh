if [ "$RES" -e "" ]; then
  RES=160x120
fi
temp=$(mktemp --suffix=".mp4")
ffmpeg -y -i "$1" -r 15 -s "$RES" -an "$temp"
./a.out -i "$temp" -o "out.vid" -c 12
rm "$temp"
