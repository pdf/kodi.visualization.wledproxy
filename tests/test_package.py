#!/usr/bin/env python3
"""Packaging checks: builds the package target and inspects both archives.

    test_package.py <cmake-binary-dir> <addon-id> <version>
"""

import os
import subprocess
import sys
import tarfile
import tempfile
import xml.etree.ElementTree as ET
import zipfile

failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print("  FAIL: " + msg)


def main():
    build, addon_id, version = sys.argv[1:4]
    subprocess.run(["cmake", "--build", build, "--target", "package_addon"], check=True,
                   stdout=subprocess.DEVNULL)
    base = os.path.join(build, "%s-%s" % (addon_id, version))
    zpath, tpath = base + ".zip", base + ".tar.gz"
    check(os.path.isfile(zpath), "zip exists")
    check(os.path.isfile(tpath), "tar.gz exists")

    with zipfile.ZipFile(zpath) as z:
        names = z.namelist()
        check(all(n.startswith(addon_id + "/") for n in names), "zip root is the add-on folder")
        for required in ("addon.xml", addon_id + ".so", "resources/settings.xml",
                         "resources/language/resource.language.en_gb/strings.po",
                         "resources/lib/configure_target.py", "LICENSE.txt"):
            check(addon_id + "/" + required in names, "zip has " + required)
        root = ET.fromstring(z.read(addon_id + "/addon.xml"))
        check(root.get("id") == addon_id, "addon.xml id")
        check(root.get("version") == version, "addon.xml version %s == %s" % (root.get("version"), version))
        ext = root.find("extension[@point='xbmc.player.musicviz']")
        check(ext is not None and ext.get("library_linux") == addon_id + ".so", "library_linux")
        platform = root.find("extension[@point='xbmc.addon.metadata']/platform")
        check(platform is not None and platform.text == "linux-aarch64", "platform")
        imports = {i.get("addon"): (i.get("minversion"), i.get("version")) for i in root.iter("import")}
        check(imports == {
            "kodi.binary.global.main": ("2.0.3", "2.0.4"),
            "kodi.binary.global.general": ("1.0.4", "1.0.6"),
            "kodi.binary.global.filesystem": ("1.1.11", "1.1.11"),
            "kodi.binary.instance.visualization": ("5.0.0", "5.0.0"),
        }, "imports %r" % imports)
        # Kodi's rule (CAddonInfo::MeetsVersion): minversion <= installed
        # version and version >= installed abi. Kodi 22.0 RC1 provides these.
        rc1 = {
            "kodi.binary.global.main": ("2.0.3", "2.0.3"),
            "kodi.binary.global.general": ("1.0.4", "1.0.5"),
            "kodi.binary.global.filesystem": ("1.1.11", "1.1.11"),
            "kodi.binary.instance.visualization": ("5.0.0", "5.0.0"),
        }
        key = lambda v: tuple(int(x) for x in v.split("."))
        for dep, (minv, ver) in imports.items():
            abi, installed = rc1[dep]
            check(key(minv) <= key(installed) and key(ver) >= key(abi), "%s works on 22.0 RC1" % dep)
        zip_names = sorted(names)

    with tarfile.open(tpath) as t:
        files = sorted(m.name for m in t.getmembers() if m.isfile())
        check(files == zip_names, "tar and zip hold the same files")
        check(all(m.name.split("/")[0] == addon_id for m in t.getmembers()), "tar root")

    # Same input, same bytes.
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "again")
        tool = os.path.join(os.path.dirname(__file__), "..", "tools", "make_package.py")
        subprocess.run([sys.executable, tool, "--stage", os.path.join(build, "stage"),
                        "--id", addon_id, "--out", out], check=True, stdout=subprocess.DEVNULL)
        for ext in (".zip", ".tar.gz"):
            with open(base + ext, "rb") as a, open(out + ext, "rb") as b:
                check(a.read() == b.read(), ext + " is reproducible")

    print("%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
