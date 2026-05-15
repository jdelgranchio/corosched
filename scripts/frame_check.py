# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""Coroutine frame-size check.

A corosched coroutine does not allocate: its frame is placement-new'd into a
caller-supplied `corosched::frame<N>` buffer (`task_frame<N>` / `co_frame<N>`).
The compiler decides the real size, and `promise_base::operator new` only checks
it at run time: an over-small buffer is `corosched::fatal(frame_too_small)` the
first time the coroutine is called, which on a controller with no debugger
attached is a silent hang. The declared N is therefore verified at build time,
from the outside, which is what this does.

Both numbers are in the DWARF that -g already emits. GCC names each coroutine's
frame struct after the ramp function's mangled name plus ".Frame":

    _ZL14actuation_taskRN9corosched5frameILj1024EEE.Frame   DW_AT_byte_size: 424
                        ^^^^^^^^^^^^^^^^^^^^^^^^^ declared    ^^^^^^^^^^^^ actual

so the mangled parameter list carries the declared N and the DIE carries the
measured size — no annotations, no registry, and every new coroutine is covered
the moment it is written. The one assumption is the library's calling
convention: a coroutine's own storage is the `corosched::frame<N>` in its
signature.

Sizes are per-build (they change with optimisation level and with any edit to
the coroutine body), so this runs as a post-action on each env's own ELF.

Failure modes are deliberately loud: finding no frames at all, or finding a
coroutine whose buffer cannot be identified, is an error rather than a pass —
otherwise a change in compiler naming would silently turn the check off.

Usage: nothing to set up. library.json names this as the library's extraScript,
so every embedded GCC build that links corosched is checked. C++ sources that
lack type-level debug info get -g2: it only adds sections to the ELF, so the
flashed image stays byte-identical. A coroutine can live in any translation
unit, including through a header, so no separately compiled file could see them
all, and a second compile would be measuring a different build.

An env can opt out (a test deliberately using an undersized frame, or a project
that also has coroutines from outside corosched):

    custom_corosched_frame_check = no

Hosted builds are skipped: Clang has no such frame types. By hand, on any ELF:

    python frame_check.py firmware.elf [objdump] [c++filt]
"""

import os
import re
import subprocess
import sys

# Fraction of the declared buffer above which a frame is reported as tight. Not
# a failure: it is a heads-up that the next edit to that coroutine may overflow.
WARN_AT = 0.80

# Keeps a level that already carries types, and rejects -g0/-g1, which do not.
_HAS_TYPES = re.compile(r"-g(gdb)?[23]?$|-gdwarf")

_DIE = re.compile(r"^\s*<\d+><[0-9a-f]+>:\s+Abbrev Number:\s+\d+\s+\((\w+)\)")
_ATTR = re.compile(r"^\s*<[0-9a-f]+>\s+(DW_AT_\w+)\s*:\s*(.*)$")
_STR = re.compile(r"^\(indirect string, offset: 0x[0-9a-f]+\):\s*(.*)$")
# The declared buffer, read off the demangled signature. Demangled rather than
# mangled because Itanium substitutions ("NS4_5frameILj256EE") make the mangled
# spelling depend on what else is in the signature; c++filt expands them all.
_DECLARED = re.compile(r"corosched::frame<(\d+)u?>")
# Fallback for names c++filt cannot demangle — a template with an `auto&`
# non-type parameter is one, and this codebase is full of them. c++filt passes
# those through unchanged, so read the frame straight out of the mangling.
_DECLARED_MANGLED = re.compile(r"5frameILj(\d+)EE")


def _objdump_from(cc):
    """Derive the toolchain's objdump from its compiler path."""
    for suffix in ("gcc", "g++"):
        if cc.endswith(suffix):
            return cc[: -len(suffix)] + "objdump"
    return "objdump"


def read_dwarf(objdump, elf):
    return subprocess.run(
        [objdump, "--dwarf=info", elf],
        capture_output=True, text=True, check=True,
    ).stdout


def collect_frames(out):
    """Every coroutine frame type in a DWARF dump, as {name: byte_size}.

    Names are deduped: the same frame appears in each CU that saw the coroutine.
    """
    frames, tag, name, size = {}, None, None, None
    for line in out.splitlines():
        die = _DIE.match(line)
        if die:
            # A new DIE starts: bank the previous one if it was a frame.
            if tag == "DW_TAG_structure_type" and name and name.endswith(".Frame"):
                frames[name] = size
            tag, name, size = die.group(1), None, None
            continue
        attr = _ATTR.match(line)
        if not attr or tag != "DW_TAG_structure_type":
            continue
        key, value = attr.group(1), attr.group(2).strip()
        if key == "DW_AT_name":
            indirect = _STR.match(value)
            name = indirect.group(1) if indirect else value
        elif key == "DW_AT_byte_size":
            size = int(value.split()[0])
    if tag == "DW_TAG_structure_type" and name and name.endswith(".Frame"):
        frames[name] = size
    return frames


def demangle(cxxfilt, names):
    """Demangle a list of symbols in one pass; falls back to the raw names."""
    try:
        out = subprocess.run(
            [cxxfilt], input="\n".join(names), capture_output=True, text=True, check=True,
        ).stdout.splitlines()
    except (OSError, subprocess.CalledProcessError):
        return list(names)
    return out if len(out) == len(names) else list(names)


def check(objdump, cxxfilt, elf, env_name):
    dwarf = read_dwarf(objdump, elf)
    if "DW_TAG_compile_unit" not in dwarf:
        print(f"frame_check[{env_name}]: {elf} has no debug info, so nothing can be read.")
        print("  Not a pass: the check is not running. Rebuild with -g2.")
        return 1
    frames = collect_frames(dwarf)
    if not frames:
        # promise_type is corosched's own name, so it still shows coroutines exist
        # if GCC renames its frame types. A project with corosched in its lib_deps
        # and no coroutine has neither.
        if "promise_type" not in dwarf:
            print(f"frame_check[{env_name}]: no coroutines in this build.")
            return 0
        print(f"frame_check[{env_name}]: coroutines but no frame types in {elf}.")
        print("  GCC may have changed how it names them, or a source lost its -g2.")
        print("  Not a pass: the check is not running.")
        print("  To opt this env out: custom_corosched_frame_check = no")
        return 1

    names = sorted(frames)
    pretties = demangle(cxxfilt, [n[: -len(".Frame")] for n in names])

    rows, bad, unchecked = [], [], []
    for mangled, pretty in zip(names, pretties):
        used = frames[mangled]
        declared = {int(n) for n in _DECLARED.findall(pretty)}
        if not declared:
            declared = {int(n) for n in _DECLARED_MANGLED.findall(mangled)}
        # One buffer per coroutine. Two different sizes in one signature means
        # the storage cannot be identified from the outside, and guessing would
        # be worse than admitting it.
        if len(declared) != 1:
            unchecked.append(pretty)
            continue
        limit = declared.pop()
        rows.append((pretty, used, limit))
        if used is None or used > limit:
            bad.append((pretty, used, limit))

    width = max((len(f"{u}/{l}") for _, u, l in rows), default=8)
    print(f"frame_check[{env_name}]: {len(rows)} coroutine frames")
    for pretty, used, limit in sorted(rows, key=lambda r: -(r[1] or 0) / r[2]):
        pct = (used or 0) / limit
        mark = "FAIL" if used is None or used > limit else ("tight" if pct >= WARN_AT else "ok")
        print(f"  {mark:>5}  {f'{used}/{limit}':>{width}}  {pct:5.0%}  {_short(pretty)}")

    for pretty in unchecked:
        print(f"  UNCHECKED  no corosched::frame<N> parameter: {_short(pretty)}")

    if bad or unchecked:
        print()
    for pretty, used, limit in bad:
        print(f"frame_check[{env_name}]: FRAME TOO SMALL — {_short(pretty)}")
        print(f"  needs {used} bytes, declared corosched::frame<{limit}>.")
        print(f"  It would trap on the first call. Raise the buffer past {used}.")
    if unchecked:
        print(f"frame_check[{env_name}]: {len(unchecked)} coroutine(s) could not be checked.")
        print("  A coroutine must take its own corosched::frame<N> as a parameter,")
        print("  or nothing can verify the buffer it was placement-new'd into.")
        print("  To opt this env out: custom_corosched_frame_check = no")
    return 1 if (bad or unchecked) else 0


def _short(pretty, limit=110):
    """Template instantiations are unreadably long; keep the head and the tail."""
    pretty = " ".join(pretty.split())
    if len(pretty) <= limit:
        return pretty
    return pretty[: limit - 24] + " ... " + pretty[-19:]


def _scons_action(target, source, env):
    del source
    cc = env.subst("$CC")
    rc = check(
        _objdump_from(cc),
        cc[:-3] + "c++filt" if cc.endswith("gcc") else "c++filt",
        str(target[0]),
        env.subst("$PIOENV"),
    )
    if rc:
        env.Exit(rc)


def _enabled(env):
    """Whether this env can and wants to be checked."""
    if not env.PioPlatform().is_embedded():
        return False
    if env.GetCompilerType() != "gcc":
        print(f"frame_check[{env.subst('$PIOENV')}]: skipped, needs GCC's frame types.")
        return False
    value = str(env.GetProjectOption("custom_corosched_frame_check", "yes"))
    return value.strip().lower() not in ("no", "false", "off", "0")


def _register_as_post_script(env, script):
    """Queue this file as one of the env's `post:` extra scripts.

    An extraScript runs while the library builders are being constructed, before
    `env.Program()` exists. AddPostAction there attaches to a placeholder executor
    that the real builder then replaces, so the check would silently never run.
    The post scripts are read after the build script, when the program node
    exists. A project already naming a frame_check.py keeps only that one.
    """
    scripts = list(env.GetProjectOption("extra_scripts", []))
    if not any(item.endswith("frame_check.py") for item in scripts):
        section = "env:" + env.subst("$PIOENV")
        env.GetProjectConfig().set(section, "extra_scripts", [*scripts, "post:" + script])


def _ensure_debug_info(env, projenv):
    """Give every C++ source the debug info the check reads.

    Reaches the objects already declared: SCons expands their commands only once
    all the scripts have run. Program link flags are for LTO, which writes the
    debug info at link time.
    """
    for target in (projenv, env, *(lb.env for lb in env.GetLibBuilders())):
        flags = [*target.get("CCFLAGS", []), *target.get("CXXFLAGS", [])]
        if not any(_HAS_TYPES.fullmatch(str(flag)) for flag in flags):
            target.Append(CXXFLAGS=["-g2"])
    if not any(_HAS_TYPES.fullmatch(str(flag)) for flag in env.get("LINKFLAGS", [])):
        env.Append(LINKFLAGS=["-g2"])


# PlatformIO entry point, reached twice: first as the library's extraScript, then
# as the post script it queued. PIOMAINPROG is set once the program node exists.
try:
    Import("env")  # noqa: F821  (injected by SCons)
except NameError:
    if __name__ == "__main__":
        if len(sys.argv) < 2:
            sys.exit("usage: frame_check.py <firmware.elf> [objdump] [c++filt]")
        _elf = sys.argv[1]
        _objdump = sys.argv[2] if len(sys.argv) > 2 else "objdump"
        _filt = sys.argv[3] if len(sys.argv) > 3 else "c++filt"
        sys.exit(check(_objdump, _filt, _elf, "cli"))
else:
    if env.get("PIOMAINPROG"):  # noqa: F821
        Import("projenv")  # noqa: F821
        _ensure_debug_info(env, projenv)  # noqa: F821
        env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", _scons_action)  # noqa: F821
    elif _enabled(env):  # noqa: F821
        Import("pio_lib_builder")  # noqa: F821
        _register_as_post_script(  # noqa: F821
            env,  # noqa: F821
            os.path.join(pio_lib_builder.path, "scripts", "frame_check.py"),  # noqa: F821
        )
