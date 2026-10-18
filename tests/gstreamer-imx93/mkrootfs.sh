#!/usr/bin/env bash
#
# Build a bootable ext4 rootfs for the GStreamer-on-i.MX93 display demo.
#
# Starts from the BSP's imx-image-core rootfs (which already ships gst-launch,
# the GStreamer core libs, glib/orc, Mesa/libdrm and a Weston that auto-starts),
# then stages the GStreamer plugin .so's the pipeline needs - the BSP builds
# them as .deb's but does not install them into any image - plus their shared
# library closure. Everything is decoded/rendered in SOFTWARE: the i.MX93 has no
# hardware JPEG/video codec (its RM has zero codec blocks), so this exercises the
# userspace media stack feeding pixels to the modelled LCDIFv3 display, not a
# codec device.
#
# Run under fakeroot so the staged tree is owned by root and mke2fs -d records
# root ownership without needing real privileges:
#
#     fakeroot bash mkrootfs.sh
#
# Inputs (env): ROOTFS (rootfs tar.zst), FEED (BSP .deb feed dir), STAGE (scratch
# dir), IMG (output ext4), PIPELINE (m1|m2), MEDIA (path to an .ogv for m2).
set -eu

ROOTFS=${ROOTFS:?rootfs tar.zst}
FEED=${FEED:?deb feed dir}
STAGE=${STAGE:?staging dir}
IMG=${IMG:?output ext4 image}
PIPELINE=${PIPELINE:-m1}
MEDIA=${MEDIA:-}

# Plugin .so's -> /usr/lib/gstreamer-1.0 (element packages).
PLUGIN_DEBS=(
    gstreamer1.0-plugins-base-videotestsrc
    gstreamer1.0-plugins-base-videoconvertscale
    gstreamer1.0-plugins-base-typefindfunctions
    gstreamer1.0-plugins-base-ogg
    gstreamer1.0-plugins-base-theora
    gstreamer1.0-plugins-bad-waylandsink
)
# Shared library closure not present in imx-image-core (glib/orc/gst-core and
# libwayland-client already are). Verified with readelf against the plugins.
LIB_DEBS=(
    libgstvideo-1.0-0 libgstaudio-1.0-0 libgstpbutils-1.0-0 libgsttag-1.0-0
    libgstallocators-1.0-0 libgstriff-1.0-0 libgstwayland-1.0-0
    libtheora libogg0
)

find_deb() {
    # The "$1"_ glob pins the exact package name (the -dbg/-dev/-src variants are
    # "$1-dbg_" etc. and do not match "$1_"), so no extra filtering is needed.
    ls "$FEED"/*/"$1"_*.deb 2>/dev/null | head -1
}

echo "[mkrootfs] extracting base rootfs"
rm -rf "$STAGE"; mkdir -p "$STAGE"
zstd -dc < "$ROOTFS" | tar -C "$STAGE" -xf -

echo "[mkrootfs] staging GStreamer plugin + library .deb's"
for pkg in "${PLUGIN_DEBS[@]}" "${LIB_DEBS[@]}"; do
    deb=$(find_deb "$pkg")
    [ -n "$deb" ] || { echo "[mkrootfs] ERROR: deb not found in feed: $pkg" >&2; exit 1; }
    dpkg-deb -x "$deb" "$STAGE"
    echo "    + ${deb##*/}"
done

echo "[mkrootfs] verifying shared-library closure (readelf)"
# Every DT_NEEDED of a staged plugin must resolve to a library that now exists
# somewhere under the staged rootfs (ld.so's trusted /usr/lib + /lib fallback).
libdirs=$(cd "$STAGE" && find usr/lib lib -maxdepth 3 -name '*.so*' -printf '%f\n' 2>/dev/null | sort -u)
missing=0
for so in "$STAGE"/usr/lib/gstreamer-1.0/*.so; do
    while read -r need; do
        case "$need" in libc.so*|libm.so*|ld-linux*|libpthread*|libdl*|librt*|libgcc*) continue;; esac
        grep -qx "$need" <<<"$libdirs" || { echo "[mkrootfs] UNRESOLVED: $need (needed by ${so##*/})" >&2; missing=1; }
    done < <(readelf -d "$so" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p')
done
[ "$missing" -eq 0 ] || { echo "[mkrootfs] ERROR: library closure incomplete; add the deb(s) above" >&2; exit 1; }
echo "    closure OK"

echo "[mkrootfs] patching weston.ini (use-g2d=false: the i.MX93 has no G2D, so"
echo "           Weston must software-render or the screen stays black)"
sed -i 's/^use-g2d=true/use-g2d=false/' "$STAGE/etc/xdg/weston/weston.ini"

echo "[mkrootfs] writing a machine-specific /etc/fstab"
# The stock fstab's "/dev/root  /  auto  defaults  1  1" line makes
# systemd-remount-fs fsck+remount root; on this bare single-ext4 SD (root is
# already mounted by the kernel via root=/dev/mmcblk0) that fails and cascades
# Local File Systems -> tmpfs mounts -> emergency mode. Drop the root line; keep
# the pseudo-fs + tmpfs entries.
cat > "$STAGE/etc/fstab" <<'EOF'
proc       /proc        proc    defaults                            0  0
devpts     /dev/pts     devpts  mode=0620,ptmxmode=0666,gid=5       0  0
tmpfs      /run         tmpfs   mode=0755,nodev,nosuid,strictatime  0  0
tmpfs      /var/volatile tmpfs  defaults                            0  0
EOF

echo "[mkrootfs] installing the gst-demo runner + service ($PIPELINE)"
# Pipeline selection. waylandsink renders into the auto-started Weston, which
# (software) composites to the LCDIFv3 scanout -> pixels on the emulated HDMI.
case "$PIPELINE" in
  m1) PIPE='videotestsrc ! video/x-raw,width=640,height=480 ! videoconvert ! waylandsink';;
  m2) PIPE='filesrc location=/usr/share/gst-demo/clip.ogv ! oggdemux ! theoradec ! videoconvert ! waylandsink';;
  *)  echo "[mkrootfs] ERROR: unknown PIPELINE=$PIPELINE" >&2; exit 1;;
esac

install -d "$STAGE/usr/bin" "$STAGE/usr/share/gst-demo"
cat > "$STAGE/usr/bin/gst-demo" <<EOF
#!/bin/sh
# Run the demo GStreamer pipeline as a Wayland client of the system Weston.
# Weston's socket is /run/wayland-0 (see weston.socket), so a client uses
# XDG_RUNTIME_DIR=/run + WAYLAND_DISPLAY=wayland-0.
export XDG_RUNTIME_DIR=/run
export WAYLAND_DISPLAY=wayland-0
export GST_DEBUG=\${GST_DEBUG:-2}
ldconfig 2>/dev/null || true
# Wait for the compositor socket (Weston starts in parallel).
i=0
while [ ! -S "\$XDG_RUNTIME_DIR/\$WAYLAND_DISPLAY" ] && [ \$i -lt 60 ]; do
    sleep 1; i=\$((i+1))
done
echo "gst-demo: starting pipeline -> $PIPE"
exec gst-launch-1.0 -e $PIPE
EOF
chmod 0755 "$STAGE/usr/bin/gst-demo"

cat > "$STAGE/etc/systemd/system/gst-demo.service" <<'EOF'
[Unit]
Description=GStreamer display demo (software pipeline -> Weston -> LCDIFv3)
After=weston.service
Wants=weston.service

[Service]
Type=simple
ExecStart=/usr/bin/gst-demo
Restart=no
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=graphical.target
EOF
ln -sf ../gst-demo.service \
    "$STAGE/etc/systemd/system/graphical.target.wants/gst-demo.service"

if [ "$PIPELINE" = m2 ]; then
    [ -n "$MEDIA" ] && [ -e "$MEDIA" ] || { echo "[mkrootfs] ERROR: m2 needs MEDIA=<clip.ogv>" >&2; exit 1; }
    cp "$MEDIA" "$STAGE/usr/share/gst-demo/clip.ogv"
    echo "    + clip.ogv ($(du -h "$MEDIA" | cut -f1))"
fi

echo "[mkrootfs] building ext4 image"
need=$(du -sm "$STAGE" | cut -f1)
need=$(( need + need/4 + 128 ))            # +25% +128MiB headroom
sz=64; while [ "$sz" -lt "$need" ]; do sz=$(( sz * 2 )); done   # next power-of-2 MiB:
# the i.MX93 uSDHC SD model requires a power-of-2 card size (2 GiB here).
rm -f "$IMG"
# ^metadata_csum: mke2fs -d can write inconsistent block/inode-bitmap checksums,
# which the guest then flags as "Filesystem failed CRC" on the first write. The
# image is a throwaway, so drop metadata checksums rather than chase the mismatch.
mkfs.ext4 -F -q -L gstdemo -O ^metadata_csum -d "$STAGE" "$IMG" "${sz}M"
e2fsck -fy "$IMG" >/dev/null 2>&1 || true   # normalise free counts before boot
echo "[mkrootfs] done: $IMG (${sz}M, pipeline=$PIPELINE)"
