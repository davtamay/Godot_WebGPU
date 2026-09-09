"""File-editing helpers for build-web-profiled.sh.

Three small jobs that shell quoting makes fragile: enabling the detection
plugin in a project.godot, curating the detected profile, and reporting the
size a strip bought.
"""

import json
import os
import re
import sys

PLUGIN_ENTRY = '"res://addons/build_profile_tool/plugin.cfg"'


def enable_plugin(path):
    """Add the detection plugin to a project's enabled editor plugins."""
    with open(path, encoding="utf-8", newline="") as f:
        text = f.read()
    if PLUGIN_ENTRY in text:
        return
    pattern = r"^enabled=PackedStringArray\((.*)\)$"
    if re.search(pattern, text, re.M):
        def merge(match):
            existing = match.group(1).strip()
            entries = PLUGIN_ENTRY if not existing else existing + ", " + PLUGIN_ENTRY
            return "enabled=PackedStringArray(%s)" % entries

        text = re.sub(pattern, merge, text, count=1, flags=re.M)
    else:
        text = text.rstrip("\n") + "\n\n[editor_plugins]\n\nenabled=PackedStringArray(%s)\n" % PLUGIN_ENTRY
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def curate(path, keep_options):
    """Re-enable build options the target needs regardless of the project.

    Detection answers "does this project use it", which is the right question
    for classes and the wrong one for a handful of build options: a web export
    needs its rendering device, a renderer and the WebGL fallback whether or
    not any scene mentions them.
    """
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    options = data.get("disabled_build_options", {})
    restored = [key for key in keep_options if key in options]
    for key in restored:
        del options[key]
    data["disabled_build_options"] = options
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent="\t", sort_keys=True)
    print("CURATE classes=%d options=%d restored=%s"
          % (len(data.get("disabled_classes", [])), len(options), ",".join(restored) or "none"))
    if options:
        print("CURATE off: %s" % ", ".join(sorted(options)))


def size(before_path, after_path):
    before = os.path.getsize(before_path)
    after = os.path.getsize(after_path)
    print("  template before %.1f MB -> now %.1f MB (%+.1f%%)"
          % (before / 1048576, after / 1048576, 100 * (after - before) / before))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    command, args = argv[1], argv[2:]
    if command == "enable" and len(args) == 1:
        enable_plugin(args[0])
    elif command == "curate" and args:
        curate(args[0], args[1:])
    elif command == "size" and len(args) == 2:
        size(args[0], args[1])
    else:
        print("usage: profile_helper.py enable <project.godot> | curate <profile> [option...] | size <a> <b>")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
