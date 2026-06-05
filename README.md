# SD Studio

A small **Qt 6** desktop app that wraps stable-diffusion.cpp's `sd-cli`, with a
GUI **and** an embedded **OpenAI-style HTTP server** sharing one generation engine.

## Features
- Prompt + **negative prompt** (works, unlike Lemonade's image API).
- Backend settings: `vulkan` / `cuda` / `cpu`, steps, CFG, width/height, seed.
- Model picker from a models directory; **Hugging Face model downloader**.
- Live preview (nearest-neighbour scaling → crisp pixels).
- Optional **cutout** → transparent PNG (native edge flood-fill; no dependencies).
- **LoRA support** + downloader — `--lora-model-dir` + `<lora:name:weight>` (e.g. `pixel-art-xl`
  on SDXL, `PixelArtRedmond` on SD-1.5). LoRA files live in `<models>/loras/`.
- **Turbo/XL-aware defaults** — auto-sets CFG + steps when an `xl`/`turbo`/`lightning` model is picked.
- Single-file model presets incl. **DreamShaper XL Turbo** (best quality) and several pixel models.
- Embedded HTTP server (pure `QTcpServer`, no extra deps):
  - `POST /v1/images/generations` → OpenAI-style `{"data":[{"b64_json":...}]}`
  - `POST /generate` → `{"image_b64":..., "seconds":...}`
  - `GET  /health`
  - Body fields: `prompt`, `negative_prompt`, `model`, `backend`, `steps`,
    `cfg_scale`, `width`/`height` or `size` ("512x512"), `seed`, `cutout`, `trigger`.

## Build
Needs Qt 6 (Widgets + Network) and the `sd-cli` from stable-diffusion.cpp
(the app auto-finds the Lemonade-bundled one under
`~/.cache/lemonade/bin/sd-cpp/<backend>/`).

**Qt Creator (easiest):** open `CMakeLists.txt`, pick the Qt 6 MSVC kit, build.

**Command line (Ninja + MSVC):**
```bat
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/msvc2022_64
cmake --build build
windeployqt --no-translations build\sd_studio.exe   :: copy Qt DLLs next to the exe
```

## Run
Launch `build\sd_studio.exe`. The HTTP server starts on port **8801** (configurable in the UI).
```powershell
curl -X POST http://127.0.0.1:8801/v1/images/generations `
  -H "Content-Type: application/json" `
  -d '{"prompt":"a candy cane","cutout":true,"size":"512x512"}'
```

## Notes
- One GPU generation runs at a time (serialized); concurrent HTTP requests get `503`.
- On an 8 GB GPU, don't keep a chat LLM resident while generating; stick to 512×512.
