#!/bin/bash

export KERNELNAME=Super

export LOCALVERSION=-SuperRyzen-V11_OC

export KBUILD_BUILD_USER=TianWalkzzMiku

export KBUILD_BUILD_HOST=NakanoMiku

export TOOLCHAIN=gcc

export DEVICES=whyred,tulip

source helper

gen_toolchain

send_msg "⏳ Start building ${KERNELNAME} ${LOCALVERSION} | DEVICES: whyred - tulip"

START=$(date +"%s")

for i in ${DEVICES//,/ }
do
	build ${i} -oldcam -overclock

	build ${i} -newcam -overclock
done

git apply oc.patch

git apply em.patch

END=$(date +"%s")

DIFF=$(( END - START ))

send_msg "✅ Build completed in $((DIFF / 60))m $((DIFF % 60))s | Linux version : $(make kernelversion) | Last commit: $(git log --pretty=format:'%s' -5)"
