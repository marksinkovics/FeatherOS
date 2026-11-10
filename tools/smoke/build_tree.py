import os


def ProgramBuilt(build_dir, arch, name):
    programs = os.path.join(build_dir, "programs")
    return os.path.exists(os.path.join(programs, f"{name}-{arch}.elf"))
