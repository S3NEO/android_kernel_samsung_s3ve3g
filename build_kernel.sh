#!/bin/bash

export ARCH=arm
export CROSS_COMPILE=/home/fcuzzocrea/bin/arm-eabi-4.8/arm-eabi/bin/
mkdir output

make -C $(pwd) O=output lineageos_s3ve3g_defconfig
make -C $(pwd) -j4 O=output

#cp output/arch/arm/boot/Image $(pwd)/arch/arm/boot/zImage
