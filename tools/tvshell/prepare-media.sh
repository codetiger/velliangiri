#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Prepare high-resolution 1280x720 baseline 4:2:0 JPEG media assets for
# Giri on Velliangiri OS (/bin/tvshell):
#   1. Converts research/gallery/*.jpeg and research/gallery/*.mov into
#      auto-oriented 1280x720 baseline 4:2:0 JPEGs + 720p video frame
#      sequences in build/dl/gallery/
#   2. Fetches high-resolution 1280x720 internet photographs for news,
#      science, weather, video, and app placeholder cards into
#      build/dl/placeholders/
#
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
GALLERY_SRC="${GALLERY_SRC:-$ROOT/research/gallery}"
GALLERY_OUT="$ROOT/build/dl/gallery"
PLACEHOLDER_OUT="$ROOT/build/dl/placeholders"
RAW_OUT="$PLACEHOLDER_OUT/.raw"

mkdir -p "$GALLERY_OUT" "$PLACEHOLDER_OUT" "$RAW_OUT"
rm -rf "$GALLERY_OUT/.frames"

download_one() {
	name="$1"
	url="$2"
	dst="$RAW_OUT/$name.jpg"
	if [ -s "$dst" ]; then
		return 0
	fi
	if curl -fsSL -A "Mozilla/5.0 (Velliangiri-OS/1.2)" --connect-timeout 8 --max-time 20 "$url" -o "$dst.tmp" 2>/dev/null && [ -s "$dst.tmp" ]; then
		mv "$dst.tmp" "$dst"
		echo "[media] Downloaded high-res internet photo: $name.jpg"
		return 0
	fi
	rm -f "$dst.tmp"
	return 1
}

download_all_placeholders() {
	download_one "dawn_ridge"      "https://images.unsplash.com/photo-1506744038136-46273834b3fb?w=1280&q=85&fm=jpg&fit=crop" || true
	download_one "weather_sky"     "https://images.unsplash.com/photo-1534088568595-a066f410bcda?w=1280&q=85&fm=jpg&fit=crop" || true
	download_one "monsoon_video"   "https://images.unsplash.com/photo-1501630834273-4b5604d2ee31?w=1280&q=85&fm=jpg&fit=crop" || true
	download_one "waterfall_video" "https://images.unsplash.com/photo-1432405972618-c60b0225b8f9?w=1280&q=85&fm=jpg&fit=crop" || true
	download_one "shola_mist"      "https://images.unsplash.com/photo-1511497584788-876760111969?w=1280&q=85&fm=jpg&fit=crop" || true
	download_one "app_chess"       "https://images.unsplash.com/photo-1529699211952-734e80c4d42b?w=1280&q=85&fm=jpg&fit=crop" || true
	download_one "app_radio"       "https://images.unsplash.com/photo-1590602847861-f357a9332bbc?w=1280&q=85&fm=jpg&fit=crop" || true
	download_one "news_world"      "https://images.unsplash.com/photo-1518770660439-4636190af475?w=1280&q=85&fm=jpg&fit=crop" || true
	download_one "news_transit"    "https://images.unsplash.com/photo-1474487548417-781cb71495f3?w=1280&q=85&fm=jpg&fit=crop" || true
	download_one "news_climate"    "https://images.unsplash.com/photo-1448375240586-882707db888b?w=1280&q=85&fm=jpg&fit=crop" || true
	download_one "news_water"      "https://images.unsplash.com/photo-1470071459604-3b5ec3a7fe05?w=1280&q=85&fm=jpg&fit=crop" || true
	download_one "sci_space"       "https://images.unsplash.com/photo-1462331940025-496dfbfc7564?w=1280&q=85&fm=jpg&fit=crop" || true
	download_one "sci_energy"      "https://images.unsplash.com/photo-1509391366360-2e959784a276?w=1280&q=85&fm=jpg&fit=crop" || true
	download_one "sci_forest"      "https://images.unsplash.com/photo-1441974231531-c6227db76b6e?w=1280&q=85&fm=jpg&fit=crop" || true
	download_one "sci_ocean"       "https://images.unsplash.com/photo-1507525428034-b723cf961d3e?w=1280&q=85&fm=jpg&fit=crop" || true
}

if [ "${1:-}" = "--download" ]; then
	download_all_placeholders
	exit 0
fi

FFMPEG=""
for cand in /opt/homebrew/bin/ffmpeg /usr/local/bin/ffmpeg "$(command -v ffmpeg 2>/dev/null || true)"; do
	if [ -n "$cand" ] && [ -x "$cand" ]; then
		FFMPEG="$cand"
		break
	fi
done

if [ -z "$FFMPEG" ]; then
	echo "error: ffmpeg is required to prepare 1280x720 baseline JPEGs" >&2
	exit 1
fi

SRGB_ICC="/System/Library/ColorSync/Profiles/sRGB Profile.icc"
TMP_STAGE="$GALLERY_OUT/.srgb_stage.jpg"
trap 'rm -f "$TMP_STAGE" /tmp/test_srgb.jpg /tmp/test_6452_out.jpg' EXIT

to_baseline_720p() {
	src="$1"
	dst="$2"
	in_img="$src"
	w=""
	h=""

	if command -v sips >/dev/null 2>&1 && [ -f "$SRGB_ICC" ]; then
		if sips --matchTo "$SRGB_ICC" -s format jpeg -s formatOptions 100 "$src" --out "$TMP_STAGE" >/dev/null 2>&1 && [ -s "$TMP_STAGE" ]; then
			in_img="$TMP_STAGE"
		fi
		w=$(sips -g pixelWidth "$in_img" 2>/dev/null | awk '/pixelWidth:/{print $2}')
		h=$(sips -g pixelHeight "$in_img" 2>/dev/null | awk '/pixelHeight:/{print $2}')
	fi

	# Preserve full vertical framing on portrait (3:4, 2:3) and near-square (8:7) photos
	if [ -n "$w" ] && [ -n "$h" ] && [ "$((w * 10))" -lt "$((h * 13))" ]; then
		"$FFMPEG" -y -hide_banner -loglevel error -i "$in_img" \
			-filter_complex "[0:v]scale=1280:720:force_original_aspect_ratio=increase:flags=lanczos,crop=1280:720,gblur=sigma=32,eq=brightness=-0.28:saturation=0.80[bg];[0:v]scale=1280:720:force_original_aspect_ratio=decrease:flags=lanczos+accurate_rnd+full_chroma_int,unsharp=3:3:0.35:3:3:0.0[fg];[bg][fg]overlay=(W-w)/2:(H-h)/2,format=yuvj420p" \
			-frames:v 1 -q:v 1 -qmin 1 -qmax 1 "$dst"
	else
		"$FFMPEG" -y -hide_banner -loglevel error -i "$in_img" \
			-vf "scale=1280:720:force_original_aspect_ratio=increase:flags=lanczos+accurate_rnd+full_chroma_int,crop=1280:720,unsharp=3:3:0.35:3:3:0.0,format=yuvj420p" \
			-frames:v 1 -q:v 1 -qmin 1 -qmax 1 "$dst"
	fi
	rm -f "$TMP_STAGE"
}

# 1. Convert research/gallery photos and videos if present
if [ -d "$GALLERY_SRC" ]; then
	echo "[media] Converting gallery photos and videos from $GALLERY_SRC ..."
	for f in "$GALLERY_SRC"/*; do
		[ -f "$f" ] || continue
		bn="$(basename "$f")"
		case "$bn" in
		*.jpeg|*.JPEG|*.jpg|*.JPG|*.png|*.PNG)
			out_jpg="$GALLERY_OUT/$bn"
			if [ "${FORCE_REBUILD:-0}" = "1" ] || [ ! -s "$out_jpg" ]; then
				to_baseline_720p "$f" "$out_jpg"
			fi
			;;
		*.mov|*.MOV|*.mp4|*.MP4)
			out_vid="$GALLERY_OUT/$bn"
			if [ ! -s "$out_vid" ]; then
				"$FFMPEG" -y -hide_banner -loglevel error -i "$f" \
					-vf "fps=24,scale=1280:720:force_original_aspect_ratio=increase:flags=lanczos,crop=1280:720,format=yuvj420p" \
					-c:v mjpeg -q:v 3 -pix_fmt yuvj420p \
					-c:a pcm_s16le -ar 48000 -ac 2 \
					-f mov "$out_vid"
			fi
			;;
		esac
	done
fi

# 2. Try downloading any missing raw internet placeholders, then convert to 1280x720 baseline 4:2:0
download_all_placeholders

prepare_placeholder() {
	name="$1"
	fallback_gal="$2"
	out="$PLACEHOLDER_OUT/$name.jpg"
	raw="$RAW_OUT/$name.jpg"

	if [ -s "$raw" ]; then
		to_baseline_720p "$raw" "$out"
		return 0
	fi
	if [ -s "$out" ]; then
		return 0
	fi
	if [ -n "$fallback_gal" ] && [ -s "$GALLERY_OUT/$fallback_gal" ]; then
		cp "$GALLERY_OUT/$fallback_gal" "$out"
	fi
}

prepare_placeholder "dawn_ridge"      "IMG_6452.jpeg"
prepare_placeholder "weather_sky"     "IMG_6418.jpeg"
prepare_placeholder "monsoon_video"   "IMG_6455.jpeg"
prepare_placeholder "waterfall_video" "IMG_9481.jpeg"
prepare_placeholder "shola_mist"      "IMG_6455.jpeg"
prepare_placeholder "app_chess"       "IMG_8734.mov"
prepare_placeholder "app_radio"       "IMG_8624.mov"
prepare_placeholder "news_world"      "IMG_8734.mov"
prepare_placeholder "news_transit"    "IMG_9368.jpeg"
prepare_placeholder "news_climate"    "IMG_6455.jpeg"
prepare_placeholder "news_water"      "DSC04988.jpeg"
prepare_placeholder "sci_space"       "IMG_8734.mov"
prepare_placeholder "sci_energy"      "IMG_6397.jpeg"
prepare_placeholder "sci_forest"      "IMG_9403.jpeg"
prepare_placeholder "sci_ocean"       "GOPR6262.jpeg"

echo "[media] Prepared $(ls -1 "$GALLERY_OUT" 2>/dev/null | wc -l | tr -d ' ') gallery items and $(ls -1 "$PLACEHOLDER_OUT"/*.jpg 2>/dev/null | wc -l | tr -d ' ') high-res placeholders."
