Import("env")


def _coerce_upload_flags(source, target, upload_env):
    flags = list(upload_env.get("UPLOADERFLAGS", []))
    if not flags:
        return

    # Ensure esptool global flags are set before the write_flash command.
    try:
        write_flash_idx = flags.index("write_flash")
    except ValueError:
        write_flash_idx = len(flags)

    if "--no-stub" not in flags:
        flags.insert(write_flash_idx, "--no-stub")
        write_flash_idx += 1

    # Disable compression to reduce serial transfer fragility on noisy links.
    flags = [flag for flag in flags if flag != "-z"]

    upload_env.Replace(UPLOADERFLAGS=flags)
    print("[upload] Applied transport guardrails: --no-stub and no compression")


env.AddPreAction("upload", _coerce_upload_flags)