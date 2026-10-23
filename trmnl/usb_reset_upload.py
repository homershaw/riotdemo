Import("env")

flags = list(env.get("UPLOADERFLAGS", []))
for i in range(len(flags) - 1):
    if flags[i] == "--before":
        flags[i + 1] = "usb_reset"
        break
else:
    flags = ["--before", "usb_reset"] + flags

env.Replace(UPLOADERFLAGS=flags)
