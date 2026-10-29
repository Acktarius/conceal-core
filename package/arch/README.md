# Arch Linux package

Requirements, dependencies, upgrade and removal rules, the workflow, and the
developer test plan live in [docs/packaging.md](../../docs/packaging.md).
This file covers only what is specific to this directory.

## Files

- `PKGBUILD`: source build of one exact commit. It installs `conceald`,
  `concealwallet`, `walletd`, and `optimizer` to `/usr/bin`, plus the license
  files to `/usr/share/licenses/conceal-core/`.
- `../test-install.sh`: install, upgrade, and removal check that the workflow
  runs in a disposable container. Do not run it on a machine you care about.

The committed `PKGBUILD` has placeholder values for `_commit` and
`sha256sums`. The workflow and the local steps below fill them in for the
commit being built. The source is a `git bundle` of that commit, so CMake can
read the commit ID for `--version`, and makepkg verifies the bundle's SHA256.

## Build locally on Arch or Omarchy

From a checkout of the commit you want, as a normal user:

```bash
sudo pacman -S --needed base-devel git cmake boost

commit=$(git rev-parse HEAD)
work=$(mktemp -d)
cp package/arch/PKGBUILD "$work/"
git bundle create "$work/conceal-core.bundle" HEAD
sum=$(sha256sum "$work/conceal-core.bundle" | cut -d' ' -f1)
sed -i \
  -e "s/^pkgrel=.*/pkgrel=0.$(date -u +%Y%m%d%H%M)/" \
  -e "s/^_commit=.*/_commit=$commit/" \
  -e "s/^sha256sums=.*/sha256sums=('$sum')/" \
  "$work/PKGBUILD"
cd "$work" && makepkg
```

`makepkg` uses two compile jobs. On a machine with more memory, set
`CCX_BUILD_JOBS`, for example `CCX_BUILD_JOBS=8 makepkg`.

Install the result with `sudo pacman -U ./conceal-core-*.pkg.tar.zst`.

## Omarchy submission

Do this only after the Arch and Omarchy rows of the test plan in
[docs/packaging.md](../../docs/packaging.md) pass.

The Omarchy package repository keeps one PKGBUILD per package. A submitted
PKGBUILD cannot depend on a bundle produced by this repository's workflow, so
the submission replaces the bundle source with a pinned upstream source for
the release tag, with a real checksum, and uses `pkgrel=1`. Keep `pkgver`,
`depends`, `makedepends`, the CMake options, and `package()` as they are here.

TODO: confirm the Omarchy repository's required metadata file and review
process with its maintainers before opening the submission.
