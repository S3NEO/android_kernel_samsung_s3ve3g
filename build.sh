#!/bin/bash
# By HASH
#

BUILD_START=$(date +"%s")
blue='\033[0;34m'
cyan='\033[0;36m'
yellow='\033[0;33m'
red='\033[0;31m'
nocol='\033[0m'

echo -e "$yellow*****************************************************"
echo "                 Galaxy Grand 2        "
echo -e "*****************************************************$nocol"

rm -rf arch/arm/boot/*.dtb
make clean && make mrproper

export CROSS_COMPILE=/home/hash/toolchain/ubertc-4.9/bin/arm-eabi-
export ARCH=arm
export SUBARCH=arm
export KBUILD_BUILD_USER="HASH"
export KBUILD_BUILD_HOST="lazy-machine"

make cyanogenmod_ms013g_defconfig
echo -e "$blue*****************************************************"
echo "           Compiling AX_Kernel         "
echo -e "*****************************************************$nocol"

make -j2

BUILD_END=$(date +"%s")
DIFF=$(($BUILD_END - $BUILD_START))
echo -e "$yellow Kernel compiled in $(($DIFF / 60)) minute(s) y $(($DIFF % 60)) seconds.$nocol"