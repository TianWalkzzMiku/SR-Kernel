#!/bin/bash

export KERNELNAME=Super

export LOCALVERSION=Ryzen-V19-EOL-Qti

export KBUILD_BUILD_USER=TianWalkzzMiku

export KBUILD_BUILD_HOST=Whyred@Sangarr

export TOOLCHAIN=clang

export DEVICES=whyred

source helper

gen_toolchain

send_msg "⏳ Start building ${KERNELNAME} ${LOCALVERSION} | DEVICES: whyred"

START=$(date +"%s")

for i in ${DEVICES//,/ }
do
	build ${i} -oldcam
done

send_msg "⏳ Start building Overclock version | DEVICES: whyred"

git apply oc.patch

for i in ${DEVICES//,/ }
do
	if [ $i == "whyred" ]
	then
		build ${i} -oldcam -overclock
	fi
done

END=$(date +"%s")

DIFF=$(( END - START ))

send_msg "✅ Build completed in $((DIFF / 60))m $((DIFF % 60))s | Linux version : $(make kernelversion) | Last commit: $(git log --pretty=format:'%s' -5)"
