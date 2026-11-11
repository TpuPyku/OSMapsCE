"""OSMapsCE's voice prompts: our own voice made by Qwen3-TTS (Apache-2.0), clips from phrases.tsv.

1. Design: a few voices from the description (VOICE) - listen, pick one:
       python make_voice.py design --lang ru --n 4 --out <dir>
2. Build: every clip of phrases.tsv in the picked voice (the Base model repeats the design clip's voice, so all the
   clips sound alike), trimmed, evened in loudness, 22050 Hz mono 16-bit WAV (the unit plays it with no decoder):
       python make_voice.py build --lang ru --ref <dir>/ru_2.wav --out ../OSMapsCE/Voice/ru

Needs: pip install qwen-tts soundfile (Python 3.10-3.12, torch); ffmpeg in PATH. The models (~9 GB) go to Hugging
Face's cache; kept on another disk - set HF_HUB_CACHE to their folder before running.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LANG = {'ru': ('Russian', 1), 'en': ('English', 2)}   # the language's name and its column in phrases.tsv
VOICE = ('A young woman in her early twenties with a bright, clear, ringing, slightly high voice. Warm, kind and '
         'polite, gently cheerful, like a friendly heroine of a fairy-tale animated film, but calm and confident. '
         'Crisp diction, moderate pace, clean studio recording, no background noise.')
SAMPLE = {'ru': 'Через триста метров поверните направо. Затем держитесь левее. Вы приехали!',
          'en': 'In three hundred meters, turn right. Then keep left. You have arrived!'}
RATE = 22050


def phrases(col):
    out = []
    for line in open(os.path.join(HERE, 'phrases.tsv'), encoding='utf-8'):
        if line.strip() and not line.startswith('#'):
            f = line.rstrip('\n').split('\t')
            out.append((f[0], f[col]))
    return out


def load(name, device):
    import torch
    from qwen_tts import Qwen3TTSModel
    # bf16 on a GPU (fp16 overflows: NaN, 2026-11-08 on a GTX 1650), fp32 on the CPU (2x slower)
    dtype = torch.bfloat16 if device.startswith('cuda') else torch.float32
    from huggingface_hub import snapshot_download
    try:   # downloaded once: from the disk (qwen-tts asks the hub every time otherwise and fails with no network)
        name = snapshot_download(name, local_files_only=True)
    except Exception:
        pass
    return Qwen3TTSModel.from_pretrained(name, device_map=device, dtype=dtype)


def finish(raw, sr, path):
    """The clip as the unit gets it: silence off both ends (a short pause kept), loudness evened, 22050 Hz mono."""
    import numpy as np
    import soundfile as sf
    raw = np.asarray(raw, dtype=np.float32)
    if not np.isfinite(raw).all():
        sys.exit('%s: the model gave NaN (try --device cpu)' % path)
    tmp = path + '.raw.wav'
    sf.write(tmp, raw, sr)
    trim = ('silenceremove=start_periods=1:start_threshold=-45dB:start_silence=0.05,areverse,'
            'silenceremove=start_periods=1:start_threshold=-45dB:start_silence=0.08,areverse,'
            'loudnorm=I=-16:TP=-1.5:LRA=7')
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', tmp, '-af', trim, '-ar', str(RATE), '-ac', '1',
                    '-c:a', 'pcm_s16le', path], check=True)
    os.remove(tmp)


def design(a):
    import torch
    m = load('Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign', a.device)
    os.makedirs(a.out, exist_ok=True)
    for k in range(1, a.n + 1):
        torch.manual_seed(k)
        wavs, sr = m.generate_voice_design(text=SAMPLE[a.lang], language=LANG[a.lang][0], instruct=VOICE)
        finish(wavs[0], sr, os.path.join(a.out, '%s_%d.wav' % (a.lang, k)))
        print('voice', k, flush=True)


def build(a):
    import torch
    m = load('Qwen/Qwen3-TTS-12Hz-%s-Base' % a.size, a.device)
    prompt = m.create_voice_clone_prompt(ref_audio=a.ref, ref_text=a.ref_text or SAMPLE[a.ref_lang or a.lang])
    os.makedirs(a.out, exist_ok=True)
    name, col = LANG[a.lang]
    for pid, text in phrases(col):
        if a.only and pid not in a.only.split(','):
            continue
        torch.manual_seed(a.seed)
        wavs, sr = m.generate_voice_clone(text=text, language=name, voice_clone_prompt=prompt)
        finish(wavs[0], sr, os.path.join(a.out, pid + '.wav'))
        print(pid, flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('mode', choices=['design', 'build'])
    p.add_argument('--lang', choices=sorted(LANG), required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--n', type=int, default=4)
    p.add_argument('--ref', help='build: the picked design clip')
    p.add_argument('--ref-lang', choices=sorted(LANG), help='build: its language, when not --lang')
    p.add_argument('--ref-text', help='build: its words, when not the design sample')
    p.add_argument('--only', help='build: just these clips (ids, comma-separated), e.g. one that came out badly')
    p.add_argument('--seed', type=int, default=1, help='build: another seed - another take')
    p.add_argument('--size', default='1.7B', choices=['0.6B', '1.7B'])
    p.add_argument('--device', default='cuda:0')
    a = p.parse_args()
    if a.mode == 'build' and not a.ref:
        sys.exit('build needs --ref')
    (design if a.mode == 'design' else build)(a)


if __name__ == '__main__':
    main()
