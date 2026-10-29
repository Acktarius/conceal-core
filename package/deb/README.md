# Debian package (Ubuntu 22.04 and 24.04)

Requirements, dependencies, upgrade and removal rules, the workflow, and the
developer test plan live in [docs/packaging.md](../../docs/packaging.md).
This file covers only what is specific to this directory.

The `.deb` is distributed through GitHub only. It is not submitted to the
Debian or Ubuntu archives, and it is not part of the Omarchy submission.

## Files

These are the `debian/` files from the
[Debian Packaging Tutorial](https://www.debian.org/doc/manuals/packaging-tutorial/packaging-tutorial):

- `control`: package metadata, build dependencies, and
  `${shlibs:Depends}` for runtime libraries.
- `rules`: `dh` with the CMake build system. It copies the four binaries to
  `/usr/bin` because upstream CMake has no install rules for them.
- `copyright`: machine-readable copyright for the project and for the bundled
  miniupnpc and parallel-hashmap code.
- `changelog`: template entry. The workflow replaces the version with the
  build version.
- `source/format`: `3.0 (native)`.

The repository has no top-level `debian/` directory. `dpkg-buildpackage`
expects one, so the build copies this directory to `debian/` first and leaves
this README out.

## Build locally on Ubuntu

From a checkout of the commit you want, on the Ubuntu release you are
targeting:

```bash
sudo apt-get update
sudo apt-get install -y build-essential python3-dev gcc g++ git cmake libboost-all-dev debhelper lintian

. /etc/os-release
version="6.7.5-0.$(date -u +%Y%m%d%H%M)+ubuntu$VERSION_ID"
rm -rf debian && cp -r package/deb debian && rm debian/README.md
sed -i "1s/^conceal-core ([^)]*) [^;]*;/conceal-core ($version) $VERSION_CODENAME;/" debian/changelog
DEB_BUILD_OPTIONS="parallel=2 noddebs" dpkg-buildpackage -b -us -uc
```

Replace `6.7.5` with the `VERSION` in `CMakeLists.txt` for the commit you are
building. The package is written to the parent directory. Install it with
`sudo apt install ../conceal-core_*_amd64.deb`. Remove the generated `debian/`
and `build/` directories afterwards; they are not tracked.
