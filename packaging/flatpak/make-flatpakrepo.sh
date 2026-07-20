#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# make-flatpakrepo.sh — write a .flatpakrepo file pointing at a published
# Flatpak repo, with the signing key's public half embedded so
# `flatpak remote-add --from` picks up trust with no separate --gpg-import
# step.
#
# Usage: make-flatpakrepo.sh REPO_URL GPG_KEY_ID OUT_FILE
#   REPO_URL    the published repo's base URL (e.g.
#               https://<owner>.github.io/<repo>/flatpak/)
#   GPG_KEY_ID  fingerprint or key id of the signing key (must be in the
#               calling gpg keyring — see .github/workflows/flatpak-build.yml)
#   OUT_FILE    where to write the .flatpakrepo file
set -e

url="$1"
keyid="$2"
out="$3"

if [ -z "$url" ] || [ -z "$keyid" ] || [ -z "$out" ]; then
	echo "usage: make-flatpakrepo.sh REPO_URL GPG_KEY_ID OUT_FILE" >&2
	exit 1
fi

gpgkey=$(gpg --export "$keyid" | base64 -w0)

cat > "$out" <<EOF
[Flatpak Repo]
Title=KRUDD Editor (self-hosted)
Url=$url
Comment=KRUDD native editor builds, self-hosted — no Flathub submission.
Description=Self-hosted Flatpak registry for the KRUDD engine's native editor (proof of life — see the editor's own docs for what it does and doesn't render yet).
Homepage=https://github.com/kruddage/engine
GPGKey=$gpgkey
EOF

echo "wrote $out"
