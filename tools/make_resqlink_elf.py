from pathlib import Path
from shutil import copy2

Import("env")


def copy_resqlink_elf(*args, **kwargs):
    project_dir = Path(str(env.subst("$PROJECT_DIR")))
    source = Path(str(env.subst("$BUILD_DIR/${PROGNAME}.elf")))
    target_dir = project_dir / "build"
    target_dir.mkdir(parents=True, exist_ok=True)
    target = target_dir / "RESQLinkDrone.elf"

    if not source.exists():
        print(f"[RESQLinkDrone] Source ELF not found: {source}")
        return

    copy2(source, target)
    print(f"[RESQLinkDrone] Copied {source} -> {target}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", copy_resqlink_elf)
