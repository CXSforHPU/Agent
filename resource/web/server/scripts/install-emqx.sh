#!/usr/bin/env bash
set -euo pipefail
image="${1:?EMQX image required}"
export PATH="/usr/local/bin:/opt/homebrew/bin:/Applications/Docker.app/Contents/Resources/bin:$PATH"
case "$(uname -s)" in
  Darwin)
    if ! command -v docker >/dev/null 2>&1; then
      command -v brew >/dev/null 2>&1 || { echo "Homebrew is required to install Docker Desktop automatically."; exit 1; }
      brew install --cask docker
    fi
    if ! docker info >/dev/null 2>&1; then open -g -a Docker; fi
    ;;
  Linux)
    if ! command -v docker >/dev/null 2>&1; then
      command -v curl >/dev/null 2>&1 || { echo "curl is required to download the official Docker installer."; exit 1; }
      installer=$(mktemp)
      trap 'rm -f "$installer"' EXIT
      curl --fail --show-error --location https://get.docker.com -o "$installer"
      if [ "$(id -u)" = 0 ]; then sh "$installer"; else sudo -n sh "$installer"; fi
    fi
    if ! docker info >/dev/null 2>&1; then
      if [ "$(id -u)" = 0 ]; then systemctl start docker; else sudo -n systemctl start docker; fi
    fi
    ;;
  *) echo "Unsupported platform: $(uname -s)"; exit 1 ;;
esac
ready=false
for ((attempt=0; attempt<60; attempt++)); do
  if [ "$(docker info --format '{{.OSType}}' 2>/dev/null || true)" = linux ]; then ready=true; break; fi
  sleep 5
done
if [ "$ready" != true ]; then
  echo "Docker Linux engine is not ready. Check first-run setup and permissions for the web server user, then retry."
  exit 1
fi
docker pull "$image"
