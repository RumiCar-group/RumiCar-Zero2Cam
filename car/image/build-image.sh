#!/bin/bash
# build-image.sh — make the standard RumiCar Zero2Cam SD-card image:
# official Raspberry Pi OS Lite (64-bit) + the rumicar-zero2cam package +
# first-boot settings (user, SSH, hostname, wireless country, config.txt).
#
#   sudo car/image/build-image.sh --deb rumicar-zero2cam_0.3.0_arm64.deb --out dist [options]
#
# Runs as root on an arm64 Debian/Ubuntu host (GitHub's ubuntu-24.04-arm
# runner, or a Raspberry Pi): the package is installed natively in a chroot
# of the mounted image, so no emulation is involved.  Needs: curl, xz, losetup
# (util-linux), mount, chroot, openssl, sha256sum.
set -euo pipefail

DEB=""
OUT="dist"
IMAGE_URL="https://downloads.raspberrypi.com/raspios_lite_arm64_latest"   # redirects to the current .img.xz
IMAGE_FILE=""            # use an already downloaded .img.xz instead of IMAGE_URL
NAME="rumicar-zero2cam"
USERNAME="rumicar"
PASSWORD="rumicar"       # documented default; the image is public and the AP is open (docs 5.1, decision A)
HOSTNAME="rumicar"
COUNTRY="JP"             # wireless regulatory domain; required for the access point
KEEP_WORK=0

usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

while [ $# -gt 0 ]; do
    case "$1" in
        --deb) DEB="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --image-url) IMAGE_URL="$2"; shift 2 ;;
        --image-file) IMAGE_FILE="$2"; shift 2 ;;
        --name) NAME="$2"; shift 2 ;;
        --user) USERNAME="$2"; shift 2 ;;
        --password) PASSWORD="$2"; shift 2 ;;
        --hostname) HOSTNAME="$2"; shift 2 ;;
        --country) COUNTRY="$2"; shift 2 ;;
        --keep-work) KEEP_WORK=1; shift ;;
        *) usage ;;
    esac
done
[ -n "$DEB" ] && [ -f "$DEB" ] || { echo "error: --deb <package.deb> is required" >&2; usage; }
[ "$(id -u)" = 0 ] || { echo "error: run as root (sudo)" >&2; exit 1; }
[ "$(uname -m)" = "aarch64" ] || { echo "error: the package is installed in a native chroot; run on an arm64 host" >&2; exit 1; }

DEB="$(readlink -f "$DEB")"
mkdir -p "$OUT"
OUT="$(readlink -f "$OUT")"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/rumicar-image.XXXXXX")"
MNT="$WORK/mnt"
LOOP=""
mounted=()

cleanup() {
    set +e
    for m in "${mounted[@]}"; do umount -l "$m" 2>/dev/null; done
    [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
    [ "$KEEP_WORK" = 1 ] || rm -rf "$WORK"
}
trap cleanup EXIT

log() { echo "==> $*"; }

# --- 1. the official image --------------------------------------------------------
if [ -n "$IMAGE_FILE" ]; then
    BASE_XZ="$(readlink -f "$IMAGE_FILE")"
    log "using $BASE_XZ"
else
    log "resolving $IMAGE_URL"
    FINAL_URL="$(curl -fsSIL -o /dev/null -w '%{url_effective}' "$IMAGE_URL")"
    log "downloading $FINAL_URL"
    BASE_XZ="$WORK/$(basename "$FINAL_URL")"
    curl -fsSL "$FINAL_URL" -o "$BASE_XZ"
    curl -fsSL "$FINAL_URL.sha256" -o "$BASE_XZ.sha256"
    log "verifying the download"
    (cd "$WORK" && sha256sum -c "$(basename "$BASE_XZ").sha256")
fi
BASE_NAME="$(basename "$BASE_XZ" .img.xz)"
IMG="$WORK/$NAME.img"
log "decompressing to $IMG"
xz -dc "$BASE_XZ" > "$IMG"

# --- 2. mount ------------------------------------------------------------------------
LOOP="$(losetup --find --show --partscan "$IMG")"
log "loop device $LOOP"
sleep 1
BOOT_PART="${LOOP}p1"
ROOT_PART="${LOOP}p2"
[ -b "$ROOT_PART" ] || { echo "error: partitions of $LOOP not found" >&2; exit 1; }
mkdir -p "$MNT"
mount "$ROOT_PART" "$MNT"; mounted+=("$MNT")
mkdir -p "$MNT/boot/firmware"
mount "$BOOT_PART" "$MNT/boot/firmware"; mounted+=("$MNT/boot/firmware")
for fs in dev proc sys; do mount --bind "/$fs" "$MNT/$fs"; mounted+=("$MNT/$fs"); done
mount --bind /dev/pts "$MNT/dev/pts"; mounted+=("$MNT/dev/pts")

# DNS inside the chroot (the image's resolv.conf is a symlink to a runtime file).
if [ -L "$MNT/etc/resolv.conf" ]; then
    RESOLV_LINK="$(readlink "$MNT/etc/resolv.conf")"
    rm -f "$MNT/etc/resolv.conf"
else
    RESOLV_LINK=""
    [ -f "$MNT/etc/resolv.conf" ] && cp "$MNT/etc/resolv.conf" "$WORK/resolv.conf.orig"
fi
cp /etc/resolv.conf "$MNT/etc/resolv.conf"

# Keep services from starting during package installation inside the chroot.
cat > "$MNT/usr/sbin/policy-rc.d" <<'POLICY'
#!/bin/sh
exit 101
POLICY
chmod +x "$MNT/usr/sbin/policy-rc.d"

in_chroot() { chroot "$MNT" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin DEBIAN_FRONTEND=noninteractive LC_ALL=C "$@"; }

# --- 3. the package ----------------------------------------------------------------
log "image: $(cat "$MNT/etc/os-release" | grep PRETTY_NAME)"
cp "$DEB" "$MNT/tmp/rumicar.deb"
log "apt-get update"
in_chroot apt-get update
log "installing $(basename "$DEB")"
in_chroot apt-get install -y --no-install-recommends /tmp/rumicar.deb
in_chroot apt-get clean
rm -f "$MNT/tmp/rumicar.deb"
in_chroot dpkg -s rumicar-zero2cam | grep -E '^(Package|Version|Status)'
in_chroot rumicar-activate show

# --- 4. first-boot settings ------------------------------------------------------
BOOTFS="$MNT/boot/firmware"
log "config.txt: appending the RumiCar settings"
if ! grep -q "RumiCar Zero2Cam" "$BOOTFS/config.txt"; then
    { echo; cat "$MNT/usr/share/rumicar/config.txt.d/rumicar.txt"; } >> "$BOOTFS/config.txt"
fi
# Bluetooth is disabled in config.txt (dtoverlay=disable-bt); its UART helper
# service would otherwise fail at every boot.
in_chroot systemctl disable hciuart 2>/dev/null || true

log "wireless country $COUNTRY (regulatory domain, needed for the access point)"
# The same edits raspi-config's do_wifi_country makes, done directly so that
# nothing touches the build host's own rfkill state.
grep -q "^${COUNTRY}[[:space:]]" "$MNT/usr/share/zoneinfo/iso3166.tab" || { echo "error: $COUNTRY is not an ISO 3166-1 alpha-2 code" >&2; exit 1; }
sed -i -e "s/\s*cfg80211.ieee80211_regdom=\S*//" -e "s/\(.*\)/\1 cfg80211.ieee80211_regdom=$COUNTRY/" "$BOOTFS/cmdline.txt"
for f in "$MNT"/var/lib/systemd/rfkill/*:wlan; do [ -e "$f" ] && echo 0 > "$f"; done
if [ -f "$MNT/var/lib/NetworkManager/NetworkManager.state" ]; then
    sed -i 's/^WirelessEnabled=.*/WirelessEnabled=true/' "$MNT/var/lib/NetworkManager/NetworkManager.state"
fi
grep -q "cfg80211.ieee80211_regdom=$COUNTRY" "$BOOTFS/cmdline.txt"
test "$(grep -c . "$BOOTFS/cmdline.txt")" = 1

log "user $USERNAME, SSH enabled, hostname $HOSTNAME"
printf '%s:%s\n' "$USERNAME" "$(openssl passwd -6 "$PASSWORD")" > "$BOOTFS/userconf.txt"
touch "$BOOTFS/ssh"
echo "$HOSTNAME" > "$MNT/etc/hostname"
sed -i "s/127\.0\.1\.1.*/127.0.1.1\t$HOSTNAME/" "$MNT/etc/hosts"
grep -q "127.0.1.1" "$MNT/etc/hosts" || printf '127.0.1.1\t%s\n' "$HOSTNAME" >> "$MNT/etc/hosts"

# A note the user sees after logging in.
cat > "$MNT/etc/motd" <<MOTD

RumiCar Zero2Cam — $(in_chroot dpkg-query -W -f='${Version}' rumicar-zero2cam)
  service:   systemctl status rumicar-core      log: journalctl -u rumicar-core -f
  versions:  rumicar-activate show               config: /etc/rumicar/core.json
  Wi-Fi AP:  SSID RumiCar (open), car address 10.42.0.1, PC ports 5600/5601
  https://github.com/RumiCar-group/RumiCar-Zero2Cam

MOTD

# --- 5. clean up the chroot and close the image --------------------------------
rm -f "$MNT/usr/sbin/policy-rc.d"
rm -f "$MNT/etc/resolv.conf"
if [ -n "$RESOLV_LINK" ]; then ln -s "$RESOLV_LINK" "$MNT/etc/resolv.conf";
elif [ -f "$WORK/resolv.conf.orig" ]; then cp "$WORK/resolv.conf.orig" "$MNT/etc/resolv.conf"; fi
sync
for ((i=${#mounted[@]}-1; i>=0; i--)); do umount "${mounted[$i]}"; done
mounted=()
losetup -d "$LOOP"; LOOP=""

# --- 6. compress and publish --------------------------------------------------
VERSION="$(dpkg-deb -f "$DEB" Version)"
STAMP="$(date -u +%Y%m%d)"
FINAL="$OUT/$NAME-$VERSION-$STAMP.img.xz"
log "compressing to $FINAL"
xz -T0 -6 -c "$IMG" > "$FINAL"
(cd "$OUT" && sha256sum "$(basename "$FINAL")" > "$(basename "$FINAL").sha256")
cat > "$OUT/$NAME-$VERSION-$STAMP.txt" <<INFO
RumiCar Zero2Cam standard image
  package:      rumicar-zero2cam $VERSION
  base image:   $BASE_NAME (Raspberry Pi OS Lite 64-bit)
  user:         $USERNAME / $PASSWORD (SSH enabled, hostname $HOSTNAME)
  Wi-Fi:        access point, SSID RumiCar (open), car 10.42.0.1, country $COUNTRY
  write with:   Raspberry Pi Imager (choose "Use custom") or xz -dc image.img.xz | dd of=/dev/sdX bs=4M
INFO
ls -l "$OUT"
log "done"
