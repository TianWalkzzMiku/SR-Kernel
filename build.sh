#!/bin/bash

export KERNELNAME=Super

export LOCALVERSION=-SuperRyzen-VIP-Edition-V6

export KBUILD_BUILD_USER=WhysDevs

export KBUILD_BUILD_HOST=NakanoMiku

export TOOLCHAIN=clang

export DEVICES=whyred,tulip

source helper

gen_toolchain

send_msg "⏳ Start building ${KERNELNAME} ${LOCALVERSION} | DEVICES: whyred - tulip"

START=$(date +"%s")

for i in ${DEVICES//,/ }
do
	build ${i} -oldcam

	build ${i} -newcam
done

END=$(date +"%s")

DIFF=$(( END - START ))

send_msg "✅ Build completed in $((DIFF / 60))m $((DIFF % 60))s | Linux version : $(make kernelversion) | Last commit: $(git log --pretty=format:'%s' -5)"
