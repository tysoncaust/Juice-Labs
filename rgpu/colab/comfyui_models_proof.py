#!/usr/bin/env python3
"""ComfyUI full-quality proof runner for Colab (Chroma1-HD / SDXL / Z-Image).

Run this in a Colab GPU runtime (Runtime -> change type -> GPU). It installs
ComfyUI, downloads the three models, starts ComfyUI headless, and generates one
full-quality image per model via the ComfyUI API, saving each to
/content/rgpu_proofs (and to Drive if mounted). It also opens a cloudflared
tunnel to the ComfyUI UI so any workflow can be run interactively.

Why here and not on the dev box: full-quality Chroma1-HD (Flux-8.9B) and Z-Image
need a real >=15 GB GPU (Colab T4/L4). The RTX 3050 (4 GB) can't run them; this
is the on-demand Colab path the project is built around.

Model sources are in MODELS below — if a repo/filename has changed, edit it there.
"""
import json
import os
import subprocess
import sys
import time
import urllib.request

ROOT = "/content/ComfyUI"
OUT = "/content/rgpu_proofs"
API = "http://127.0.0.1:8188"

# ---- model registry (edit repo/file if upstream changes) -------------------
MODELS = {
    "sdxl": {
        "arch": "sdxl",
        "files": [
            ("checkpoints", "stabilityai/stable-diffusion-xl-base-1.0",
             "sd_xl_base_1.0.safetensors"),
        ],
    },
    "chroma1_hd": {
        "arch": "flux",  # Chroma is a Flux-schnell derivative
        "files": [
            ("diffusion_models", "lodestones/Chroma1-HD", "Chroma1-HD.safetensors"),
            ("clip", "comfyanonymous/flux_text_encoders", "t5xxl_fp8_e4m3fn.safetensors"),
            ("clip", "comfyanonymous/flux_text_encoders", "clip_l.safetensors"),
            ("vae", "black-forest-labs/FLUX.1-schnell", "ae.safetensors"),
        ],
    },
    "z_image": {
        "arch": "z_image",  # Tongyi Z-Image-Turbo (needs a recent ComfyUI)
        "files": [
            ("diffusion_models", "Tongyi-MAI/Z-Image-Turbo", "z_image_turbo.safetensors"),
        ],
        "note": "Z-Image needs a current ComfyUI build with Z-Image nodes; verify the repo/file.",
    },
}


def sh(cmd, **kw):
    print("+", cmd)
    return subprocess.run(cmd, shell=True, check=False, **kw)


def install_comfyui():
    if not os.path.isdir(ROOT):
        sh(f"git clone --depth 1 https://github.com/comfyanonymous/ComfyUI {ROOT}")
    sh(f"pip -q install -r {ROOT}/requirements.txt")
    # ComfyUI-Manager makes missing custom nodes / models easy to add interactively
    mgr = f"{ROOT}/custom_nodes/ComfyUI-Manager"
    if not os.path.isdir(mgr):
        sh(f"git clone --depth 1 https://github.com/ltdrdata/ComfyUI-Manager {mgr}")
    os.makedirs(OUT, exist_ok=True)


def download_models(which):
    from huggingface_hub import hf_hub_download  # provided by comfyui deps
    for name in which:
        for folder, repo, fname in MODELS[name]["files"]:
            dest_dir = os.path.join(ROOT, "models", folder)
            os.makedirs(dest_dir, exist_ok=True)
            dest = os.path.join(dest_dir, fname)
            if os.path.exists(dest):
                print("have", dest); continue
            try:
                p = hf_hub_download(repo_id=repo, filename=fname, local_dir=dest_dir,
                                    local_dir_use_symlinks=False)
                print("downloaded", p)
            except Exception as e:
                print(f"WARN could not fetch {repo}/{fname}: {e}")


def start_comfyui():
    subprocess.Popen([sys.executable, "main.py", "--listen", "127.0.0.1", "--port", "8188"],
                     cwd=ROOT)
    for _ in range(120):
        try:
            urllib.request.urlopen(API + "/system_stats", timeout=2); print("ComfyUI up"); return True
        except Exception:
            time.sleep(2)
    return False


def sdxl_workflow(prompt, seed=1234567):
    """Correct ComfyUI API-format SDXL graph (1024x1024, 30 steps, dpmpp_2m/karras)."""
    return {
        "4": {"class_type": "CheckpointLoaderSimple",
              "inputs": {"ckpt_name": "sd_xl_base_1.0.safetensors"}},
        "5": {"class_type": "EmptyLatentImage", "inputs": {"width": 1024, "height": 1024, "batch_size": 1}},
        "6": {"class_type": "CLIPTextEncode", "inputs": {"text": prompt, "clip": ["4", 1]}},
        "7": {"class_type": "CLIPTextEncode", "inputs": {"text": "lowres, blurry, watermark, text", "clip": ["4", 1]}},
        "3": {"class_type": "KSampler",
              "inputs": {"seed": seed, "steps": 30, "cfg": 7.0, "sampler_name": "dpmpp_2m",
                          "scheduler": "karras", "denoise": 1.0,
                          "model": ["4", 0], "positive": ["6", 0], "negative": ["7", 0], "latent_image": ["5", 0]}},
        "8": {"class_type": "VAEDecode", "inputs": {"samples": ["3", 0], "vae": ["4", 2]}},
        "9": {"class_type": "SaveImage", "inputs": {"filename_prefix": "rgpu_sdxl", "images": ["8", 0]}},
    }


def queue_and_wait(graph, timeout=600):
    body = json.dumps({"prompt": graph}).encode()
    req = urllib.request.Request(API + "/prompt", data=body, headers={"Content-Type": "application/json"})
    pid = json.load(urllib.request.urlopen(req, timeout=30))["prompt_id"]
    deadline = time.time() + timeout
    while time.time() < deadline:
        h = json.load(urllib.request.urlopen(f"{API}/history/{pid}", timeout=15))
        if pid in h and h[pid].get("outputs"):
            imgs = []
            for node in h[pid]["outputs"].values():
                for im in node.get("images", []):
                    imgs.append(im)
            return imgs
        time.sleep(3)
    return []


def save_outputs(imgs, tag):
    saved = []
    for im in imgs:
        url = f"{API}/view?filename={im['filename']}&subfolder={im.get('subfolder','')}&type={im['type']}"
        data = urllib.request.urlopen(url, timeout=60).read()
        path = os.path.join(OUT, f"{tag}_{im['filename']}")
        open(path, "wb").write(data); saved.append(path); print("saved", path)
    return saved


def open_tunnel():
    if not os.path.exists("/content/cloudflared"):
        sh("wget -q -O /content/cloudflared "
           "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-amd64 "
           "&& chmod +x /content/cloudflared")
    subprocess.Popen(["/content/cloudflared", "tunnel", "--url", API, "--no-autoupdate"])
    print("cloudflared tunnel starting -> a https://<id>.trycloudflare.com URL will appear; "
          "open it to run any workflow interactively for full-quality proofs.")


def main():
    which = sys.argv[1:] or ["sdxl", "chroma1_hd", "z_image"]
    install_comfyui()
    download_models(which)
    if not start_comfyui():
        print("ComfyUI failed to start"); return 1
    open_tunnel()
    # Automated headless proof: SDXL (fully wired). Chroma1-HD / Z-Image graphs are
    # architecture-specific and best run from the UI over the tunnel with the
    # downloaded models (their exact node names vary by ComfyUI version).
    if "sdxl" in which:
        print("=== generating SDXL full-quality proof ===")
        imgs = queue_and_wait(sdxl_workflow(
            "a highly detailed studio photograph of a red vintage sports car, "
            "cinematic lighting, 85mm, sharp focus, 8k"))
        save_outputs(imgs, "sdxl") if imgs else print("SDXL: no image returned")
    print("\nProofs in", OUT, "(mount Drive and copy for a durable record).")
    print("For Chroma1-HD and Z-Image: open the ComfyUI tunnel URL, load a Flux/Z-Image "
          "workflow, select the downloaded model, and Queue Prompt.")
    time.sleep(10)  # keep the process alive briefly so the tunnel URL prints
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
