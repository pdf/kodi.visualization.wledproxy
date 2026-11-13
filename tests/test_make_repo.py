#!/usr/bin/env python3
"""Tests for tools/make_repo.py and the repository add-on in repo/."""

import hashlib
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import make_repo  # noqa: E402

failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print("  FAIL: " + msg)


def fake_zip(folder, addon_id, version, extra=b""):
    path = os.path.join(folder, "%s-%s-%d.zip" % (addon_id, version, len(os.listdir(folder))))
    xml = ('<?xml version="1.0" encoding="UTF-8"?>\n'
           '<addon id="%s" version="%s" name="T &amp; T"><extension point="xbmc.python.script"/></addon>\n'
           % (addon_id, version)).encode()
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(addon_id + "/addon.xml", xml)
        z.writestr(addon_id + "/payload", extra)
    return path


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def checksum_file_ok(path):
    with open(path + ".sha256") as f:
        word = f.read().split()[0]
    return word == sha(path)


def real_repo_zip(tmp):
    out = os.path.join(tmp, "realrepo")
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "make_package.py"),
                    "--stage", os.path.join(ROOT, "repo"), "--id", make_repo.REPO_ID,
                    "--out", out, "--zip-only"], check=True, stdout=subprocess.DEVNULL)
    return out + ".zip"


def test_layout_and_index(tmp):
    zips = os.path.join(tmp, "zips")
    os.makedirs(zips)
    old = fake_zip(zips, "visualization.wledproxy", "0.1.0", b"old")
    new = fake_zip(zips, "visualization.wledproxy", "0.2.0", b"new")
    repo = real_repo_zip(tmp)
    repo_again = os.path.join(zips, "repo-copy.zip")
    with open(repo, "rb") as a, open(repo_again, "wb") as b:
        b.write(a.read())  # same repository zip attached to two releases
    site = os.path.join(tmp, "site")
    built = make_repo.build(site, [new, repo, old, repo_again])

    check(built == [(make_repo.REPO_ID, "1.0.0"), ("visualization.wledproxy", "0.1.0"),
                    ("visualization.wledproxy", "0.2.0")], "built %r" % built)
    for addon_id, version in built:
        p = os.path.join(site, "piers", addon_id, "%s-%s.zip" % (addon_id, version))
        check(os.path.isfile(p), "published " + p)
        check(os.path.isfile(p) and checksum_file_ok(p), "checksum of " + p)
    check(sha(os.path.join(site, "piers", "visualization.wledproxy", "visualization.wledproxy-0.1.0.zip")) == sha(old),
          "old version kept unchanged")

    index = os.path.join(site, "piers", "addons.xml")
    check(checksum_file_ok(index), "addons.xml.sha256 matches")
    root = ET.parse(index).getroot()
    check(root.tag == "addons", "addons root")
    listed = [(a.get("id"), a.get("version")) for a in root.findall("addon")]
    check(listed == built, "addons.xml lists every version %r" % listed)
    check(root.find("addon[@id='visualization.wledproxy']").get("name") == "T & T", "entities survive")

    top = os.path.join(site, make_repo.REPO_ID + ".zip")
    check(os.path.isfile(top) and sha(top) == sha(repo), "fixed-name repository zip")
    with open(os.path.join(site, "index.html")) as f:
        page = f.read()
    check('href="repository.visualization.wledproxy.zip"' in page, "index links the repository zip")
    check("visualization.wledproxy-0.2.0.zip" in page, "index lists versions")
    check(os.path.isfile(os.path.join(site, ".nojekyll")), ".nojekyll")

    # Rebuilding replaces the tree: nothing stale survives.
    stale = os.path.join(site, "piers", "stale.txt")
    open(stale, "w").close()
    make_repo.build(site, [new, repo])
    check(not os.path.exists(stale), "tree rebuilt from scratch")
    check(not os.path.exists(os.path.join(site, "piers", "visualization.wledproxy",
                                          "visualization.wledproxy-0.1.0.zip")),
          "only the given zips are published")


def test_refuses_replacing_a_version(tmp):
    zips = os.path.join(tmp, "zips2")
    os.makedirs(zips)
    a = fake_zip(zips, "visualization.wledproxy", "0.1.0", b"first build")
    b = fake_zip(zips, "visualization.wledproxy", "0.1.0", b"second build")
    repo = real_repo_zip(tmp)
    try:
        make_repo.build(os.path.join(tmp, "site2"), [repo, a, b])
        check(False, "same version with different contents must fail")
    except make_repo.RepoError as e:
        check("already published" in str(e), "error names the problem: %s" % e)
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "make_repo.py"),
                        "--out", os.path.join(tmp, "site3"), repo, a, b],
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    check(r.returncode == 1, "command exits 1")


def test_rejects_bad_zips(tmp):
    zips = os.path.join(tmp, "zips3")
    os.makedirs(zips)
    repo = real_repo_zip(tmp)
    two_roots = os.path.join(zips, "two.zip")
    with zipfile.ZipFile(two_roots, "w") as z:
        z.writestr("a/addon.xml", '<addon id="a" version="1"/>')
        z.writestr("b/x", "")
    mismatch = os.path.join(zips, "mismatch.zip")
    with zipfile.ZipFile(mismatch, "w") as z:
        z.writestr("a/addon.xml", '<addon id="b" version="1"/>')
    for bad in (two_roots, mismatch):
        try:
            make_repo.build(os.path.join(tmp, "site4"), [repo, bad])
            check(False, "accepted " + bad)
        except make_repo.RepoError:
            pass
    try:
        make_repo.build(os.path.join(tmp, "site5"), [fake_zip(zips, "x.y", "1.0.0")])
        check(False, "a tree without the repository add-on must fail")
    except make_repo.RepoError:
        pass


def test_repository_addon():
    root = ET.parse(os.path.join(ROOT, "repo", make_repo.REPO_ID, "addon.xml")).getroot()
    check(root.get("id") == make_repo.REPO_ID, "repository id")
    ext = root.find("extension[@point='xbmc.addon.repository']")
    dirs = ext.findall("dir")
    check(len(dirs) == 1, "one dir")
    d = dirs[0]
    base = "https://pdf.github.io/kodi.visualization.wledproxy/piers"
    check(d.get("minversion") == "21.90.0" and d.get("maxversion") == "22.89.0", "Piers version range")
    check(d.findtext("info") == base + "/addons.xml", "info url")
    check(d.find("checksum").get("verify") == "sha256", "checksum verify")
    check(d.findtext("checksum") == base + "/addons.xml.sha256", "checksum url")
    check(d.findtext("datadir") == base, "datadir")
    check(d.findtext("hashes") == "sha256", "hashes")
    check(root.findtext("extension[@point='xbmc.addon.metadata']/platform") == "all", "platform all")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        test_layout_and_index(tmp)
        test_refuses_replacing_a_version(tmp)
        test_rejects_bad_zips(tmp)
    test_repository_addon()
    print("%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
