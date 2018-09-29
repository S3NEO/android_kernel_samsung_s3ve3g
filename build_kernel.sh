#!/bin/bash

export ARCH=arm
export CROSS_COMPILE=~/bin/arm-eabi-4.9/arm-linux-androideabi/bin/
mkdir output

make -C $(pwd) O=output lineageos_s3ve3g_defconfig
make -C $(pwd) -j4 O=output

#cp output/arch/arm/boot/Image $(pwd)/arch/arm/boot/zImage
