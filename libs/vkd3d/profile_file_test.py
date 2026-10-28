"""amdgpu-wddm: build and run the host test of the machine profile file (profile_file_test.c).

    python libs/vkd3d/profile_file_test.py --clang-cl <clang-cl.exe> [--work <empty work dir>]

Builds the test with profile_file.c of this tree, once as the release flavour and once as the lab flavour
(AMDGPU_WDDM_LAB_FLAVOUR), and runs both. No package ships the test.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument("--clang-cl", required=True)
ap.add_argument("--work")
a = ap.parse_args()

here = os.path.abspath(os.path.dirname(__file__))
root = os.path.abspath(os.path.join(here, "..", ".."))
work = os.path.abspath(a.work) if a.work else tempfile.mkdtemp(prefix="vkd3d-profile-")
os.makedirs(work, exist_ok=True)
sources = [os.path.join(here, "profile_file_test.c"), os.path.join(here, "profile_file.c")]
flags = ["/nologo", "/MT", "/O1", "/Z7", "/W3", "-DVKD3D_PROFILE_FILE_TEST", "-D_CRT_SECURE_NO_WARNINGS",
         "-I" + here, "-I" + os.path.join(root, "include"), "-I" + os.path.join(root, "include", "private"),
         "-Wno-unused-function"]

failed = False
for flavour, defines in (("release", []), ("lab", ["-DAMDGPU_WDDM_LAB_FLAVOUR=1"])):
    out = os.path.join(work, flavour)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    objs = []
    for src in sources:
        obj = os.path.join(out, os.path.splitext(os.path.basename(src))[0] + ".obj")
        r = subprocess.run([a.clang_cl, "/c"] + flags + defines + [src, "/Fo" + obj], capture_output=True, text=True)
        if r.returncode:
            print(r.stdout[-6000:], r.stderr[-3000:])
            sys.exit(f"{flavour}: {os.path.basename(src)} does not compile")
        objs.append(obj)
    exe = os.path.join(out, "profile_file_test.exe")
    r = subprocess.run([a.clang_cl, "/nologo", "/MT"] + objs + ["/Fe" + exe, "/link", "advapi32.lib"],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-6000:], r.stderr[-3000:])
        sys.exit(f"{flavour}: the test does not link")
    r = subprocess.run([exe, os.path.join(out, "profiles")], capture_output=True, text=True, cwd=out)
    print(f"== {flavour}")
    print(r.stdout.rstrip())
    failed |= r.returncode != 0
sys.exit(1 if failed else 0)
