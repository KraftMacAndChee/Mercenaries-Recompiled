"""Read-only correlation of a retail ADPCM slice with captured host PCM.

Use a verified wavebank offset/length and observed hardware pitch. This does
not infer cue identity from a nearby VOICE_ON line, or claim audibility from
allocation success. The decoder is compiled from the production source and
emits the same first64 frames per Xbox ADPCM block as the APU.
"""
import argparse
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile

import numpy as np

ROOT = Path(__file__).resolve().parents[2]


def decode_adpcm(data, channels):
    if channels not in (1, 2) or not data or len(data) % (36*channels):
        raise ValueError("Expected whole mono/stereo Xbox ADPCM blocks")
    source = (ROOT / "src/apu/apu_state.h").read_text(encoding="utf-8")
    start = source.index("static inline int adpcm_decode_block(")
    end = source.index("\n}\n", start)+3
    wrapper = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <io.h>
'''+source[start:end]+r'''
int main(int argc,char**argv) {
 int ch=atoi(argv[1]); unsigned char block[72]; int16_t samples[130];
 if(ch!=1&&ch!=2)return 2;
 _setmode(_fileno(stdin),_O_BINARY); _setmode(_fileno(stdout),_O_BINARY);
 for(;;) {
  size_t got=fread(block,1,36*ch,stdin);if(!got)break;
  if(got!=36*ch||adpcm_decode_block(samples,block,got,ch)!=65)return 3;
  if(fwrite(samples,sizeof(int16_t),64*ch,stdout)!=64*ch)return 4;
 }
 return 0;
}
'''
    compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
    with tempfile.TemporaryDirectory(prefix="merc-voice-correlation-") as directory:
        src, exe = Path(directory)/"decode.c", Path(directory)/"decode.exe"
        src.write_text(wrapper, encoding="utf-8")
        build = subprocess.run([compiler,"-std=c11","-O2",str(src),"-o",str(exe)],
                               capture_output=True,text=True)
        if build.returncode:
            raise RuntimeError(build.stderr)
        result = subprocess.run([str(exe),str(channels)],input=data,
                                capture_output=True,timeout=15)
        if result.returncode:
            raise ValueError("Invalid ADPCM block or decoder error")
    return np.frombuffer(result.stdout,dtype="<i2").reshape(-1,channels).astype(np.float64)


def best_match(recording, reference):
    """Pearson correlation and fitted gain at every valid sample alignment."""
    if recording.ndim!=1 or reference.ndim!=1 or not 1<len(reference)<=len(recording):
        raise ValueError("Expected nonempty 1-D recording longer than reference")
    reference = reference-reference.mean()
    energy = float(reference@reference)
    if energy<=0:
        raise ValueError("Reference is silent")
    size=1 << (len(recording)+len(reference)-2).bit_length()
    product=np.fft.irfft(np.fft.rfft(recording,size)*
                         np.fft.rfft(reference[::-1],size),size)
    numerator=product[len(reference)-1:len(recording)]
    sums=np.concatenate(([0.0],np.cumsum(recording)))
    squares=np.concatenate(([0.0],np.cumsum(recording*recording)))
    n=len(reference)
    local=squares[n:]-squares[:-n]-(sums[n:]-sums[:-n])**2/n
    denominator=np.sqrt(np.maximum(local,0)*energy)
    scores=np.divide(numerator,denominator,out=np.zeros_like(numerator),where=denominator>1e-12)
    index=int(np.argmax(np.abs(scores)))
    return {"sample":index,"correlation":float(scores[index]),
            "fitted_gain":float(numerator[index]/energy)}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wavebank",type=Path)
    parser.add_argument("recording",type=Path)
    parser.add_argument("--offset",type=lambda x:int(x,0),required=True)
    parser.add_argument("--bytes",type=lambda x:int(x,0),required=True)
    parser.add_argument("--channels",type=int,choices=(1,2),default=1)
    parser.add_argument("--pitch",type=int,required=True)
    parser.add_argument("--start",type=float,required=True,help="Host PCM search start in seconds")
    parser.add_argument("--duration",type=float,required=True,help="Bounded search window, up to60 seconds")
    args=parser.parse_args()
    if not (0<=args.offset and 0<args.bytes<=4*1024*1024 and
            -32768<=args.pitch<=32767 and math.isfinite(args.start) and args.start>=0 and
            math.isfinite(args.duration) and 0<args.duration<=60):
        parser.error("Invalid bounded read/search range")
    with args.wavebank.open("rb") as stream:
        stream.seek(args.offset); data=stream.read(args.bytes)
    if len(data)!=args.bytes:
        parser.error("Wavebank slice extends past EOF")
    decoded=decode_adpcm(data,args.channels)
    step=2**(args.pitch/4096)
    count=math.ceil(len(decoded)/step)
    if count>48000*60:
        parser.error("Resampled reference exceeds60 seconds")
    reference=np.interp(np.arange(count)*step,np.arange(len(decoded)),decoded.mean(axis=1))
    raw=np.memmap(args.recording,dtype="<i2",mode="r")
    if len(raw)%2:
        parser.error("Recording must be stereo signed16 PCM")
    first=round(args.start*48000); last=first+round(args.duration*48000)
    recording=np.asarray(raw.reshape(-1,2)[first:last],dtype=np.float64).mean(axis=1)
    result=best_match(recording,reference)
    result.update({"start_seconds":args.start+result["sample"]/48000,
                   "reference_seconds":len(reference)/48000,
                   "source_frames":len(decoded),"pitch":args.pitch})
    print(json.dumps(result,indent=2))


if __name__=="__main__":
    main()
