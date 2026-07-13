#!/bin/bash
set -e

REPO="vrchester/tupac"
BINARY="tupac"
INSTALL_DIR="/usr/local/bin"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}TUPAC - Package Manager${NC}"
echo ""

# check dependencies
for cmd in curl tar; do
    if ! command -v "$cmd" &>/dev/null; then
        echo -e "${RED}Error: $cmd is required but not installed.${NC}"
        exit 1
    fi
done

# detect architecture
ARCH=$(uname -m)
case "$ARCH" in
    x86_64)  ARCH="x86_64" ;;
    aarch64) ARCH="aarch64" ;;
    armv7l)  ARCH="armv7l" ;;
    *)
        echo -e "${RED}Error: Unsupported architecture: $ARCH${NC}"
        exit 1
        ;;
esac

OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ASSET="${BINARY}-${OS}-${ARCH}"

# detect latest release
echo -n "Detecting latest release... "
LATEST=$(curl -sL "https://api.github.com/repos/${REPO}/releases/latest" | grep '"tag_name"' | cut -d'"' -f4)
if [ -z "$LATEST" ]; then
    echo -e "${RED}failed${NC}"
    echo "Could not fetch latest release. Check the repository URL."
    exit 1
fi
echo -e "${GREEN}${LATEST}${NC}"

# download
URL="https://github.com/${REPO}/releases/download/${LATEST}/${ASSET}"
echo -n "Downloading ${ASSET}... "

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

HTTP_CODE=$(curl -sL -w "%{http_code}" -o "${TMPDIR}/${BINARY}" "$URL")
if [ "$HTTP_CODE" != "200" ]; then
    # try tarball fallback
    URL="https://github.com/${REPO}/releases/download/${LATEST}/${ASSET}.tar.gz"
    HTTP_CODE=$(curl -sL -w "%{http_code}" -o "${TMPDIR}/${BINARY}.tar.gz" "$URL")
    if [ "$HTTP_CODE" != "200" ]; then
        echo -e "${RED}failed (HTTP ${HTTP_CODE})${NC}"
        echo "Binary not found for ${OS}-${ARCH}."
        echo "URL: $URL"
        exit 1
    fi
    cd "$TMPDIR" && tar xzf "${BINARY}.tar.gz" && cd -
fi

chmod +x "${TMPDIR}/${BINARY}"

# install
echo -n "Installing to ${INSTALL_DIR}/${BINARY}... "
if [ -w "$INSTALL_DIR" ] 2>/dev/null; then
    cp "${TMPDIR}/${BINARY}" "${INSTALL_DIR}/${BINARY}"
else
    sudo cp "${TMPDIR}/${BINARY}" "${INSTALL_DIR}/${BINARY}"
fi
echo -e "${GREEN}done${NC}"

echo ""
echo -e "${GREEN}TUPAC installed successfully!${NC}"
echo "Run it with: ${CYAN}tupac${NC}"
