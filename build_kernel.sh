#!/bin/bash
# From build_msm8974pro_kernel.sh; credits to nc.chaudhary@samsung.com
# Adapted and modified by xXPR0T0TYPEXx for s3ve3g

BUILD_KERNEL_DIR=$(pwd)

# Export arm architeture (32-bit) and cross compiler
export ARCH=arm
export CROSS_COMPILE=../arm-linux-androideabi-4.9/bin/arm-linux-androideabi-

# Default Python version is 2.7
mkdir -p bin
ln -sf /usr/bin/python2.7 ./bin/python
export PATH=$BUILD_KERNEL_DIR/bin:$PATH

# Output directory path
OUTPUT_DIR=../output_MM

# Initialize fields
BOARD_KERNEL_PAGESIZE=2048
BUILD_JOB_NUMBER=`grep processor /proc/cpuinfo|wc -l`
DTBTOOL=$BUILD_KERNEL_DIR/tools/dtbTool
DTC=$OUTPUT_DIR/scripts/dtc/dtc
DTS_NAMES=msm8226-sec-s3ve3geur-r
INSTALLED_DTIMAGE_TARGET=${OUTPUT_DIR}/arch/arm/boot/dt.img
KERNEL_ZIMG=$OUTPUT_DIR/arch/arm/boot/zImage


# FUNCTIONS
# Clean up kernel source
CLEAN()
{
    echo ""
	echo "================================="
	echo $1
	echo "================================="
	echo ""

    make clean
	make mrproper

	if [ $2 == 0 ]; then
	    if [ -d $OUTPUT_DIR ]; then
		    rm -rf $OUTPUT_DIR
		    echo "output directory removed"
		else
		    echo "WARNING: output directory does not exist"
		fi
	fi

	echo ""
	echo "================================="
	echo "kernel source cleaned up"
	echo "================================="
	echo ""
}

# Append dtb to zImage
APPEND_DTB()
{
	for DTS_FILE in `ls ${BUILD_KERNEL_DIR}/arch/arm/boot/dts/msm8226/${DTS_NAMES}*.dts`
	do
		DTB_FILE=${DTS_FILE%.dts}.dtb
		DTB_FILE=$OUTPUT_DIR/arch/arm/boot/${DTB_FILE##*/}
		ZIMG_FILE=${DTB_FILE%.dtb}-zImage

		echo ""
		echo "dts : $DTS_FILE"
		echo "dtb : $DTB_FILE"
		echo "out : $ZIMG_FILE"
		echo ""

		$DTC -p 1024 -O dtb -o $DTB_FILE $DTS_FILE
		cat $KERNEL_ZIMG $DTB_FILE > $ZIMG_FILE
	done
}

# Build dt image
BUILD_DTIMAGE_TARGET()
{
	echo ""
	echo "================================="
	echo "BUILDING DT IMAGE"
	echo "================================="
	echo ""
	echo "DT image target : $INSTALLED_DTIMAGE_TARGET"

	if ! [ -e $DTBTOOL ] ; then
		if ! [ -d $BUILD_KERNEL_DIR/out/host/linux-x86/bin ] ; then
			mkdir -p $BUILD_KERNEL_DIR/out/host/linux-x86/bin
		fi
	fi

	echo "$DTBTOOL -o $INSTALLED_DTIMAGE_TARGET -s $BOARD_KERNEL_PAGESIZE \
		-p $BUILD_KERNEL_DIR/scripts/dtc/ $OUTPUT_DIR/arch/arm/boot/"
        $DTBTOOL -o $INSTALLED_DTIMAGE_TARGET -s $BOARD_KERNEL_PAGESIZE \
		-p $OUTPUT_DIR/scripts/dtc/ $OUTPUT_DIR/arch/arm/boot/

	chmod a+r $INSTALLED_DTIMAGE_TARGET

	echo ""
	echo "================================="
	echo "BUILDING DT IMAGE FINISHED"
	echo "================================="
	echo ""
}

# Build zImage variant
BUILD_ZIMAGE()
{
	echo ""
	echo "=============================================="
	echo "BUILDING KERNEL STARTED"
	echo "=============================================="
	echo ""

	# Create output directory
	if [ ! -d $OUTPUT_DIR ]; then
	    mkdir $OUTPUT_DIR
	fi

	make -C $BUILD_KERNEL_DIR -j$BUILD_JOB_NUMBER O=$OUTPUT_DIR msm8226-sec_defconfig VARIANT_DEFCONFIG=msm8226-sec_s3ve3g_eur_defconfig SELINUX_DEFCONFIG=selinux_defconfig
	make -C $BUILD_KERNEL_DIR -j$BUILD_JOB_NUMBER O=$OUTPUT_DIR

	APPEND_DTB
	BUILD_DTIMAGE_TARGET

	cp $OUTPUT_DIR/arch/arm/boot/Image $BUILD_KERNEL_DIR/arch/arm/boot/zImage

	echo ""
	echo "=============================================="
	echo "BUILDING KERNEL FINISHED"
	echo "=============================================="
	echo ""
}

# MAIN FUNCTION
rm -rf ./build.log
(
	echo "=============================================="
	echo "Do you wish to clean up kernel source first?"
	echo "=============================================="
	select options in "Yes, remove output too" "Yes" "No" "Cancel"; do
        case $options in
            "Yes, remove output too")
			CLEAN "Cleaning up and removing output..." 0
			break
			;;
			"Yes") 
			CLEAN "Cleaning up..." 1
			break
			;;
            "No") 
			echo "proceeding without cleaning up..."
			break
			;;
			"Cancel") 
			echo "Script aborted..."
			exit
			;;
        esac
    done

	START_TIME=`date +%s`

	BUILD_ZIMAGE

	END_TIME=`date +%s`

	let "ELAPSED_TIME=$END_TIME-$START_TIME"
	echo "Total compile time is $ELAPSED_TIME seconds"
) 2>&1	 | tee -a ./build.log
