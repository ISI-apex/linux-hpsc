# One has to customize CROSS_COMPILE for one's system
CROSS_COMPILE=aarch64-poky-linux- make ARCH=arm64 -j4 V=1
