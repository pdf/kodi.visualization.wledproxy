#!/usr/bin/env python3
"""Builds the published Kodi repository tree (GitHub Pages) from add-on zips.

    make_repo.py --out SITE ZIP [ZIP ...]

Pass every zip that should be published: those attached to all earlier
GitHub releases, plus the new release's. The tree is rebuilt from scratch
each time, so the zips are the only source of truth and old versions stay
available for Kodi's "Versions" list (rollback).

Writes:

    SITE/piers/addons.xml               every version of every add-on
    SITE/piers/addons.xml.sha256
    SITE/piers/<id>/<id>-<version>.zip  Kodi's default download path
    SITE/piers/<id>/<id>-<version>.zip.sha256
    SITE/repository.visualization.wledproxy.zip   latest repository add-on
    SITE/index.html

Refuses (exit 1) when two zips carry the same add-on version with different
contents: a published version is never replaced. Identical duplicates (the
repository add-on attached to several releases) are fine. Standard library
only.
"""

import argparse
import hashlib
import html
import os
import re
import shutil
import sys
import xml.etree.ElementTree as ET
import zipfile

REPO_ID = "repository.visualization.wledproxy"
DIR = "piers"


class RepoError(Exception):
    pass


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def version_key(v):
    # Dotted numeric versions, as this project uses; anything else sorts last.
    parts = []
    for p in re.split(r"[.~+-]", v):
        parts.append((0, int(p)) if p.isdigit() else (1, p))
    return parts


def read_addon(path):
    """Returns (id, version, addon.xml bytes) of an add-on zip."""
    try:
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            roots = {n.split("/", 1)[0] for n in names}
            if len(roots) != 1:
                raise RepoError("%s: zip must hold exactly one add-on folder, found %s"
                                % (path, sorted(roots)))
            root = roots.pop()
            xml_name = root + "/addon.xml"
            if xml_name not in names:
                raise RepoError("%s: no %s" % (path, xml_name))
            data = z.read(xml_name)
    except zipfile.BadZipFile as e:
        raise RepoError("%s: %s" % (path, e))
    try:
        addon = ET.fromstring(data)
    except ET.ParseError as e:
        raise RepoError("%s: addon.xml: %s" % (path, e))
    addon_id, version = addon.get("id"), addon.get("version")
    if addon.tag != "addon" or not addon_id or not version:
        raise RepoError("%s: addon.xml lacks id or version" % path)
    if addon_id != root:
        raise RepoError("%s: folder %s does not match id %s" % (path, root, addon_id))
    return addon_id, version, data


def collect(zips):
    """Maps (id, version) to (zip path, sha256, addon.xml bytes)."""
    found = {}
    for path in zips:
        addon_id, version, data = read_addon(path)
        digest = sha256_of(path)
        key = (addon_id, version)
        if key in found:
            if found[key][1] != digest:
                raise RepoError(
                    "%s %s is already published with different contents (%s and %s); "
                    "bump the version instead of replacing it" % (addon_id, version, found[key][0], path))
            continue
        found[key] = (path, digest, data)
    return found


def addon_element_text(data):
    """addon.xml without its XML declaration, for embedding in addons.xml."""
    text = data.decode("utf-8")
    text = re.sub(r"^\s*<\?xml[^>]*\?>\s*", "", text)
    return text.strip()


def write_checksum(path, digest):
    # sha256sum format; Kodi reads the first word.
    with open(path + ".sha256", "w", encoding="ascii", newline="\n") as f:
        f.write("%s  %s\n" % (digest, os.path.basename(path)))


INDEX = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WLED visualization proxy repository</title>
<style>
  body {{ font-family: sans-serif; max-width: 40rem; margin: 2rem auto; padding: 0 1rem; line-height: 1.5; }}
  code {{ background: #eee; padding: 0 .2em; }}
</style>
</head>
<body>
<h1>WLED visualization proxy repository</h1>
<p>Kodi repository for <code>visualization.wledproxy</code>, a visualization add-on that runs another visualization and sends WLED audio sync packets. For Kodi 22 (Piers) on 64-bit ARM Linux.</p>
<ol>
  <li>Settings &gt; System &gt; Add-ons &gt; Unknown sources: on.</li>
  <li>Download <a href="{repo_zip}">{repo_zip}</a> (version {repo_version}) and install it with Add-ons &gt; Install from zip file.</li>
  <li>Add-ons &gt; Install from repository &gt; WLED visualization proxy repository &gt; Visualisations &gt; WLED visualization proxy &gt; Install.</li>
</ol>
<p>Published versions:</p>
<ul>
{versions}
</ul>
<p>Source: <a href="https://github.com/pdf/kodi.visualization.wledproxy">github.com/pdf/kodi.visualization.wledproxy</a></p>
</body>
</html>
"""


def build(out, zips):
    found = collect(zips)
    if not any(a == REPO_ID for a, _ in found):
        raise RepoError("no %s zip given" % REPO_ID)

    if os.path.exists(out):
        shutil.rmtree(out)
    piers = os.path.join(out, DIR)
    os.makedirs(piers)

    ordered = sorted(found, key=lambda k: (k[0], version_key(k[1])))
    entries = []
    for addon_id, version in ordered:
        src, digest, data = found[(addon_id, version)]
        folder = os.path.join(piers, addon_id)
        os.makedirs(folder, exist_ok=True)
        dest = os.path.join(folder, "%s-%s.zip" % (addon_id, version))
        shutil.copyfile(src, dest)
        write_checksum(dest, digest)
        entries.append(addon_element_text(data))

    index = '<?xml version="1.0" encoding="UTF-8"?>\n<addons>\n' + "\n".join(entries) + "\n</addons>\n"
    ET.fromstring(index.encode("utf-8"))  # must stay well-formed
    index_path = os.path.join(piers, "addons.xml")
    with open(index_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(index)
    write_checksum(index_path, sha256_of(index_path))

    repo_version = max((v for a, v in found if a == REPO_ID), key=version_key)
    repo_zip = REPO_ID + ".zip"
    shutil.copyfile(found[(REPO_ID, repo_version)][0], os.path.join(out, repo_zip))

    versions = "\n".join(
        '  <li><a href="{d}/{i}/{i}-{v}.zip">{i} {v}</a></li>'.format(
            d=DIR, i=html.escape(a), v=html.escape(v))
        for a, v in ordered)
    with open(os.path.join(out, "index.html"), "w", encoding="utf-8", newline="\n") as f:
        f.write(INDEX.format(repo_zip=repo_zip, repo_version=html.escape(repo_version),
                             versions=versions))
    # GitHub Pages: serve files as they are (no Jekyll).
    open(os.path.join(out, ".nojekyll"), "w").close()
    return ordered


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", required=True, help="site folder to (re)create")
    p.add_argument("zips", nargs="+", help="add-on zips to publish")
    a = p.parse_args()
    try:
        for addon_id, version in build(a.out, a.zips):
            print("%s %s" % (addon_id, version))
    except RepoError as e:
        print("make_repo: " + str(e), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
