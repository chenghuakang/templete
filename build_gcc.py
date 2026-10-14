#!/usr/bin/env python3
"""GCC/Clang fallback build for environments without CMake. CMake remains the primary entry point."""
import argparse, shutil, subprocess
from pathlib import Path
ap = argparse.ArgumentParser()
ap.add_argument('--output', default='build/bin')
ap.add_argument('--cxx', default='g++')
args = ap.parse_args()
root = Path(__file__).resolve().parent
out = Path(args.output).resolve(); out.mkdir(parents=True, exist_ok=True)
obj = out.parent / 'objects'; obj.mkdir(exist_ok=True)
common = ['-O2', '-Wall', '-Wextra', '-ffp-contract=off', '-fno-fast-math', '-I'+str(root/'include'), '-I'+str(root/'satpred/include')]
def cxx(*a): subprocess.run([args.cxx, *map(str, a)], check=True)
# Optional CUDA G backend (SATPRED_CUDA=AUTO semantics): build only when nvcc is present.
cuda_obj = None; cuda_link = []
nvcc = shutil.which('nvcc')
if nvcc:
    for arch in ('-arch=native', '-arch=sm_75', None):
        cmd = [nvcc, '-std=c++17', '-O2', '--fmad=false', '-I'+str(root/'satpred/include')]
        if arch: cmd.append(arch)
        cmd += ['-c', str(root/'satpred/src/cuda_candidates.cu'), '-o', str(obj/'cuda_candidates.o')]
        if subprocess.run(cmd, capture_output=True).returncode == 0:
            cuda_obj = obj/'cuda_candidates.o'
            home = Path(nvcc).resolve().parent.parent
            cuda_link = [cuda_obj, '-L'+str(home/'lib64'), '-lcudart']
            break
defs = ['-DSP_CUDA=1'] if cuda_obj else []
cxx('-std=c++17', *common, *defs, '-Werror', '-c', root/'satpred/src/core.cpp', '-o', obj/'core.o')
for app in ['satpred', 'satnet', 'satread', 'netread', 'satdemo', 'orbit_engine']:
    cxx('-std=c++17', *common, '-DAPP_NAME="'+app+'"', root/'satpred/apps/main.cpp', obj/'core.o', *cuda_link, '-o', out/app)
new = ['-std=c++20', '-Wno-deprecated-declarations', *common]
cxx(*new, root/'src/main.cpp', root/'src/simulation.cpp', '-o', out/'hypersim')
cxx(*new, root/'src/trace_main.cpp', '-o', out/'hsn-traceroute')
cxx(*new, root/'tests/test_traceroute.cpp', '-o', out/'test_traceroute')
cxx(*new, root/'tests/test_options.cpp', '-o', out/'test_options')
shutil.copytree(root/'satpred/examples', out/'examples', dirs_exist_ok=True)
print('built', out, ('cuda' if cuda_obj else 'cpu-only'))
