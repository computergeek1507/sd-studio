import glob, os, subprocess
from io import BytesIO
from rembg import remove, new_session
from PIL import Image

HERE = r"D:\sd-models\sd-studio"
MODEL = r"D:\sd-models\Public-Prompts-Pixel-Model.safetensors"
sd = glob.glob(os.path.expanduser("~/.cache/lemonade/bin/sd-cpp/vulkan/**/sd*.exe"), recursive=True)[0]
raw = os.path.join(HERE, "icon_raw.png")

print("generating icon art...", flush=True)
subprocess.run([sd, "-m", MODEL, "--vae-tiling", "--diffusion-fa",
    "-p", "pixelsprite, a single colorful retro camera, vibrant, centered, solid white background",
    "-n", "scenery, landscape, background, text, frame, blurry, multiple",
    "--cfg-scale", "7", "--steps", "26", "-W", "512", "-H", "512", "-s", "8",
    "-o", raw], check=True)

print("cutout + building .ico...", flush=True)
cut = remove(open(raw, "rb").read(), session=new_session("u2netp"))
img = Image.open(BytesIO(cut)).convert("RGBA")
img = img.crop(img.getbbox())                       # trim to content
w, h = img.size
side = max(w, h)
m = int(side * 0.08)                                # small margin
canvas = Image.new("RGBA", (side + 2 * m, side + 2 * m), (0, 0, 0, 0))
canvas.paste(img, ((side - w) // 2 + m, (side - h) // 2 + m), img)

ico = os.path.join(HERE, "sd_studio.ico")
canvas.save(ico, format="ICO", sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)])
canvas.resize((256, 256), Image.NEAREST).save(os.path.join(HERE, "sd_studio.png"))
print("saved", ico, flush=True)
