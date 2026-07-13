#!/bin/bash
set -e

REPO="ByteChesterX/tupac"
BINARY="tupac"
INSTALL_DIR="/usr/local/bin"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}TUPAC - Package Manager${NC}"
echo ""

# check dependencies
for cmd in curl; do
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
ASSET="${BINARY}-${OS}-${ARCH}.tar.gz"

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# try downloading latest release
URL="https://github.com/${REPO}/releases/latest/download/${ASSET}"
echo -n "Downloading ${ASSET}... "

HTTP_CODE=$(curl -sL -w "%{http_code}" -o "${TMPDIR}/${ASSET}" "$URL")
if [ "$HTTP_CODE" = "200" ]; then
    echo -e "${GREEN}ok${NC}"
else
    echo -e "${RED}failed (HTTP ${HTTP_CODE})${NC}"
    echo "Could not download from: $URL"
    echo ""
    echo "You can manually download from:"
    echo "  https://github.com/${REPO}/releases"
    exit 1
fi

# extract
cd "$TMPDIR"
tar xzf "${ASSET}"
chmod +x "${BINARY}"
cd -

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
