#!/bin/sh
# Install gdscript-lsp from a GitHub release.
#
#   curl -fsSL https://raw.githubusercontent.com/brohd11/gdscript-standalone-lsp/main/install.sh | sh
#
# Environment:
#   BIN_DIR=$HOME/.local/bin  executable destination
#   VERSION=v1.2.3           release to install (default: latest stable)
#
# Options:
#   --modify-path            update the shell rc file without prompting
#   --no-modify-path         never modify shell rc files

set -eu

REPO="brohd11/gdscript-standalone-lsp"
BINARY="gdscript-lsp"
SUPPORTED="macos-arm64 linux-x64"

BIN_DIR="${BIN_DIR:-$HOME/.local/bin}"
VERSION="${VERSION:-latest}"
PATH_MODE=auto

for arg in "$@"; do
	case "$arg" in
		--no-modify-path) PATH_MODE=never ;;
		--modify-path) PATH_MODE=always ;;
		-h|--help)
			cat <<EOF
install $BINARY into \$BIN_DIR (default: \$HOME/.local/bin)

  BIN_DIR=<prefix>/bin  executable destination
  VERSION=<tag>         release to install (default: latest stable)
  --modify-path         update the shell rc file without prompting
  --no-modify-path      never modify shell rc files

The bundled API metadata is installed under <prefix>/share/gdscript-lsp.
EOF
			exit 0 ;;
		*) echo "unknown option: $arg" >&2; exit 2 ;;
	esac
done

die() { echo "error: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

os=$(uname -s | tr '[:upper:]' '[:lower:]')
arch=$(uname -m)

case "$os" in
	darwin) os=macos ;;
	linux) ;;
	*) die "unsupported OS: $os (Windows: use install.ps1 from https://github.com/$REPO)" ;;
esac

case "$arch" in
	x86_64|amd64) arch=x64 ;;
	arm64|aarch64) arch=arm64 ;;
	*) die "unsupported architecture: $arch" ;;
esac

target="$os-$arch"
case " $SUPPORTED " in
	*" $target "*) ;;
	*) die "no $target build is published for $BINARY
  supported: $SUPPORTED
  build from source: https://github.com/$REPO" ;;
esac

asset="$BINARY-$target.tar.gz"
if [ "$VERSION" = latest ]; then
	release_url="https://github.com/$REPO/releases/latest/download"
else
	release_url="https://github.com/$REPO/releases/download/$VERSION"
fi

have curl || die "curl is required"
have tar || die "tar is required"
if have sha256sum; then
	hash_command=sha256sum
elif have shasum; then
	hash_command=shasum
else
	die "sha256sum or shasum is required"
fi

prefix=$(dirname "$BIN_DIR")
data_dir="$prefix/share/gdscript-lsp"
doc_dir="$prefix/share/doc/gdscript-lsp"
mkdir -p "$BIN_DIR" "$data_dir" "$doc_dir" || die "cannot create the install directories"
[ -w "$BIN_DIR" ] || die "$BIN_DIR is not writable (set BIN_DIR to a directory you own)"
[ -w "$data_dir" ] || die "$data_dir is not writable"
[ -w "$doc_dir" ] || die "$doc_dir is not writable"

echo "downloading $BINARY ($target, $VERSION)"

tmp=$(mktemp -d)
binary_stage=""
api_stage=""
readme_stage=""
notices_stage=""
cleanup() {
	[ -z "$binary_stage" ] || rm -f "$binary_stage"
	[ -z "$api_stage" ] || rm -f "$api_stage"
	[ -z "$readme_stage" ] || rm -f "$readme_stage"
	[ -z "$notices_stage" ] || rm -f "$notices_stage"
	rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

curl -fsSL -o "$tmp/$asset" "$release_url/$asset" \
	|| die "download failed: $release_url/$asset
  (check https://github.com/$REPO/releases for available versions)"
curl -fsSL -o "$tmp/SHA256SUMS" "$release_url/SHA256SUMS" \
	|| die "checksum download failed: $release_url/SHA256SUMS"

matches=$(awk -v name="$asset" '{ file=$2; sub(/^\*/, "", file); if (file == name) count++ } END { print count+0 }' "$tmp/SHA256SUMS")
[ "$matches" -eq 1 ] || die "SHA256SUMS does not contain exactly one entry for $asset"
expected=$(awk -v name="$asset" '{ file=$2; sub(/^\*/, "", file); if (file == name) { print $1; exit } }' "$tmp/SHA256SUMS")
case "$hash_command" in
	sha256sum) actual=$(sha256sum "$tmp/$asset" | awk '{ print $1 }') ;;
	shasum) actual=$(shasum -a 256 "$tmp/$asset" | awk '{ print $1 }') ;;
esac
[ "$actual" = "$expected" ] || die "checksum verification failed for $asset"

mkdir "$tmp/unpacked"
tar -xzf "$tmp/$asset" -C "$tmp/unpacked" || die "could not extract $asset"

package="$tmp/unpacked/$BINARY-$target"
source_binary="$package/bin/$BINARY"
source_api="$package/share/gdscript-lsp/godot-4.6-extension-api.json"
source_readme="$package/share/doc/gdscript-lsp/README.md"
source_notices="$package/share/doc/gdscript-lsp/THIRD_PARTY_NOTICES.md"
[ -f "$source_binary" ] || die "archive did not contain bin/$BINARY"
[ -f "$source_api" ] || die "archive did not contain the bundled Godot API metadata"
[ -f "$source_readme" ] || die "archive did not contain README.md"
[ -f "$source_notices" ] || die "archive did not contain THIRD_PARTY_NOTICES.md"

# Copy into temporary files on the destination filesystem, then rename them into
# place. A failed download, checksum, extraction, or copy leaves the old install.
binary_stage=$(mktemp "$BIN_DIR/.$BINARY.XXXXXX")
api_stage=$(mktemp "$data_dir/.godot-4.6-extension-api.json.XXXXXX")
readme_stage=$(mktemp "$doc_dir/.README.md.XXXXXX")
notices_stage=$(mktemp "$doc_dir/.THIRD_PARTY_NOTICES.md.XXXXXX")
cp "$source_binary" "$binary_stage" || die "could not stage $BINARY"
cp "$source_api" "$api_stage" || die "could not stage the Godot API metadata"
cp "$source_readme" "$readme_stage" || die "could not stage README.md"
cp "$source_notices" "$notices_stage" || die "could not stage THIRD_PARTY_NOTICES.md"
chmod +x "$binary_stage"
mv -f "$readme_stage" "$doc_dir/README.md"
readme_stage=""
mv -f "$notices_stage" "$doc_dir/THIRD_PARTY_NOTICES.md"
notices_stage=""
mv -f "$api_stage" "$data_dir/godot-4.6-extension-api.json"
api_stage=""
mv -f "$binary_stage" "$BIN_DIR/$BINARY"
binary_stage=""

echo "installed -> $BIN_DIR/$BINARY"
echo "metadata  -> $data_dir/godot-4.6-extension-api.json"

export_line="export PATH=\"\$PATH:$BIN_DIR\""
marker="# added by install.sh -- $BIN_DIR on PATH"
shell_name=$(basename "${SHELL:-}")

case "$shell_name" in
	zsh|bash|sh|ksh|ksh93|mksh|dash|ash) posix_syntax=1 ;;
	*) posix_syntax=0 ;;
esac

rc_file() {
	case "$shell_name" in
		zsh) echo "$HOME/.zshrc" ;;
		bash)
			if [ "$os" = macos ]; then
				for candidate in "$HOME/.bash_profile" "$HOME/.bash_login" "$HOME/.profile"; do
					[ -f "$candidate" ] && { echo "$candidate"; return; }
				done
				echo "$HOME/.bash_profile"
			else
				echo "$HOME/.bashrc"
			fi ;;
		*) echo "" ;;
	esac
}

print_manual() {
	if [ "$posix_syntax" -eq 1 ]; then
		echo "add it with:"
		echo "  $export_line"
	else
		echo "add $BIN_DIR to PATH using your shell's configuration"
	fi
}

add_to_path() {
	rc=$1
	if [ -f "$rc" ] && grep -qF "$BIN_DIR" "$rc"; then
		echo "$rc already references $BIN_DIR -- leaving it alone"
		return
	fi
	printf '\n%s\n%s\n' "$marker" "$export_line" >> "$rc" || die "could not write to $rc"
	echo "added to $rc -- open a new shell, or run:"
	echo "  $export_line"
}

case ":$PATH:" in
	*":$BIN_DIR:"*) ;;
	*)
		echo
		echo "$BIN_DIR is not on your PATH, so '$BINARY' will not be runnable by name."
		rc=$(rc_file)
		if [ -n "$rc" ] && [ -L "$rc" ]; then
			link_target=$(readlink "$rc" 2>/dev/null || echo "?")
			echo "note: $rc is a symlink -> $link_target"
		fi
		if [ "$PATH_MODE" = never ] || [ -z "$rc" ]; then
			print_manual
		elif [ "$PATH_MODE" = always ]; then
			add_to_path "$rc"
		elif (: < /dev/tty) 2>/dev/null; then
			printf 'Add it to %s? [y/N] ' "$rc"
			reply=""
			read -r reply < /dev/tty || reply=""
			case "$reply" in
				[yY]|[yY][eE][sS]) add_to_path "$rc" ;;
				*) echo "skipped."; print_manual ;;
			esac
		else
			print_manual
		fi ;;
esac
