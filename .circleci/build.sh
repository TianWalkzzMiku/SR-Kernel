:#!/bin/bash

export KERNELNAME=SRK

export LOCALVERSION=Petriks-V22

export KBUILD_BUILD_USER=WhysDev

export KBUILD_BUILD_HOST=NakanoMiku

export TOOLCHAIN=gcc

export DEVICES=whyred,tulip,lavender,a26x

source helper

gen_toolchain

send_msg "⏰ Start building ${KERNELNAME} ${LOCALVERSION} | DEVICES: whyred - tulip - lavender - wayne - jasmine"

START=$(date +"%s")

for i in ${DEVICES//,/ }
do
	build ${i} -oldcam

	build ${i} -newcam
done

send_msg "⏰ Start building Overclock version | DEVICES: whyred - tulip"

git apply oc.patch

git apply em.patch

for i in ${DEVICES//,/ }
do
	if [ $i == "whyred" ] || [ $i == "tulip" ]
	then
		build ${i} -oldcam -overclock

		build ${i} -newcam -overclock
	fi
done

END=$(date +"%s")

DIFF=$(( END - START ))

send_msg "☑️ Build completed in $((DIFF / 60))m $((DIFF % 60))s | Linux version : $(make kernelversion) | Last commit: $(git log --pretty=format:'%s' -5)"
