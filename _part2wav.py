import struct, sys, glob, os
os.chdir(os.path.dirname(os.path.abspath(__file__)))
parts = sorted(glob.glob("recordings/*.pcm.part"), key=os.path.getmtime)
if not parts:
    print("no .part"); sys.exit(1)
src_path = parts[-1]
src = open(src_path, "rb").read()
wav_path = os.path.splitext(src_path)[0] + ".wav"
hdr = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", len(src)+36, b"WAVE", b"fmt ",
                  16, 1, 1, 16000, 32000, 2, 16, b"data", len(src))
open(wav_path, "wb").write(hdr + src)
print("converted:", wav_path, "samples:", len(src)//2, "dur_s:", round(len(src)/32000, 1))
