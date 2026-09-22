#!/usr/bin/env python3
"""Fetch pinned dependencies locally; no packages are installed system-wide."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parent
DEPS = ROOT / ".deps"
CACHE = DEPS / "downloads"
REV = "dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284"


def fetch(name, url, digest):
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / name
    if not path.exists():
        temporary = path.with_suffix(".part")
        print(f"Downloading {name}", flush=True)
        subprocess.run(["curl", "-L", "--fail", "--retry", "2", "--max-time", "300",
                        url, "-o", str(temporary)], check=True)
        temporary.replace(path)
    with path.open("rb") as stream:
        actual = hashlib.file_digest(stream, "sha256").hexdigest()
    if actual != digest:
        raise RuntimeError(f"Checksum mismatch for {path}; remove it and retry")
    return path


def copy_member(archive, member, destination):
    """Only copy regular files to explicit destinations (never archive links)."""
    if member.isfile():
        destination.parent.mkdir(parents=True, exist_ok=True)
        with archive.extractfile(member) as source, destination.open("wb") as out:
            shutil.copyfileobj(source, out)


def extract_deb(path, selections):
    process = subprocess.Popen(["dpkg-deb", "--fsys-tarfile", str(path)], stdout=subprocess.PIPE)
    try:
        with tarfile.open(fileobj=process.stdout, mode="r|") as archive:
            for member in archive:
                name = member.name.removeprefix("./")
                for prefix, dest in selections:
                    if name == prefix or name.startswith(prefix + "/"):
                        relative = name[len(prefix):].lstrip("/")
                        if ".." in Path(relative).parts:
                            raise RuntimeError("Unsafe archive path")
                        copy_member(archive, member, dest / relative if relative else dest)
    finally:
        process.stdout.close()
    if process.wait():
        raise RuntimeError(f"Could not extract {path}")


def main():
    if (DEPS / ".ready").exists():
        print("Doom dependencies already prepared.")
        return
    engine = fetch("doomgeneric.tar.gz",
                   f"https://codeload.github.com/ozkl/doomgeneric/tar.gz/{REV}",
                   "1bd3f7f26220494159a38d71f2847ec81b58d6bbd7c7c8d81b08993018001148")
    with tarfile.open(engine) as archive:
        for member in archive:
            parts = Path(member.name).parts[1:]
            if not parts or ".." in parts:
                continue
            copy_member(archive, member, DEPS / "doomgeneric" / Path(*parts))
    libc = fetch("picolibc.deb",
                 "https://archive.ubuntu.com/ubuntu/pool/universe/p/picolibc/"
                 "picolibc-riscv64-unknown-elf_1.8.6-2_all.deb",
                 "9563bbe39bbdf4eda1970ced8c54e5df47a112d91427fc4fc38100428fd23a9b")
    prefix = "usr/lib/picolibc/riscv64-unknown-elf/"
    extract_deb(libc, [(prefix + "include", DEPS / "picolibc/include"),
                       (prefix + "lib/rv32im/ilp32", DEPS / "picolibc/lib/rv32im/ilp32"),
                       ("usr/share/doc/picolibc-riscv64-unknown-elf/copyright",
                        DEPS / "picolibc/COPYRIGHT")])
    gcc = fetch("gcc.deb",
                "https://archive.ubuntu.com/ubuntu/pool/universe/g/gcc-riscv64-unknown-elf/"
                "gcc-riscv64-unknown-elf_13.2.0-11ubuntu1+12_amd64.deb",
                "47d4670801c391c65513e2002a19280c6bbdba158f5a467ac5100bff813de0e3")
    extract_deb(gcc, [("usr/lib/gcc/riscv64-unknown-elf/13.2.0/rv32im/ilp32/libgcc.a",
                       DEPS / "gcc/libgcc.a"),
                      ("usr/share/doc/gcc-riscv64-unknown-elf/copyright",
                       DEPS / "gcc/COPYRIGHT")])
    data = fetch("freedoom.zip",
                 "https://github.com/freedoom/freedoom/releases/download/v0.13.0/"
                 "freedoom-0.13.0.zip",
                 "3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59")
    with zipfile.ZipFile(data) as archive:
        for name in ("freedoom1.wad", "COPYING.txt"):
            with archive.open("freedoom-0.13.0/" + name) as source, (DEPS / name).open("wb") as out:
                shutil.copyfileobj(source, out)
    (DEPS / ".ready").write_text(REV + "\n")
    print("Ready: Doomgeneric, RV32IM Picolibc/libgcc, and Freedoom Phase 1.")


if __name__ == "__main__":
    main()
