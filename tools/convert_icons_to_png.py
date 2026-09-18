"""Convert LVGL .c image arrays to PNG files for LittleFS migration."""
import os, re, sys
from PIL import Image

SRC = r"C:\Users\xiao1\Desktop\esp32\display_audio_photo\main\ui\icons"
DST = r"C:\Users\xiao1\Desktop\esp32\display_audio_photo\littlefs_content\icons"
os.makedirs(DST, exist_ok=True)

files = {
    "image_wifi.c": "wifi.png",
    "image_sun.c": "sun.png",
    "image_cloud.c": "cloud.png",
    "image_temp.c": "temp.png",
    "image_humidity.c": "humidity.png",
    "image_wind.c": "wind.png",
    "image_sunrise.c": "sunrise.png",
    "image_rain.c": "rain.png",
}

for cfile, pngfile in files.items():
    path = os.path.join(SRC, cfile)
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    w = int(re.search(r"\.header\.w\s*=\s*(\d+)", text).group(1))
    h = int(re.search(r"\.header\.h\s*=\s*(\d+)", text).group(1))
    cf = re.search(r"\.header\.cf\s*=\s*(LV_COLOR_FORMAT_\w+)", text).group(1)
    # extract all hex bytes in the map array
    m = re.search(r"\[\]\s*=\s*\{(.*?)\};", text, re.DOTALL)
    bytes_str = m.group(1)
    vals = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", bytes_str)]
    if cf == "LV_COLOR_FORMAT_ARGB8888":
        # BGRA? LVGL ARGB8888 is actually B,G,R,A in memory
        img = Image.frombytes("RGBA", (w, h), bytes(vals), "raw", "BGRA", 0, 1)
    elif cf == "LV_COLOR_FORMAT_RGB565":
        img = Image.frombytes("RGB;16B", (w, h), bytes(vals), "raw", "RGB;16B", 0, 1)
    else:
        print(f"skip {cfile}: unknown cf {cf}")
        continue
    out = os.path.join(DST, pngfile)
    img.save(out, "PNG", optimize=True)
    print(f"{cfile} {w}x{h} {cf} -> {pngfile} ({os.path.getsize(out)//1024}KB)")
