#!/bin/bash

export KERNELNAME=Super

export LOCALVERSION=-SuperRyzen-V12_HyperOC_VIP_EDITION

export KBUILD_BUILD_USER=TianWalkzzMiku

export KBUILD_BUILD_HOST=NakanoMiku

export TOOLCHAIN=gcc

export DEVICES=whyred,tulip,lavender

source helper

gen_toolchain

send_msg "Start building..."

START=$(date +"%s")

for i in ${DEVICES//,/ }
do
	build ${i} -oldcam

	build ${i} -newcam
done

send_msg "Build OC..."

for i in ${DEVICES//,/ }
do
	if [ $i == "whyred" ] || [ $i == "tulip" ]
	then
		build ${i} -oldcam -overclock

		build ${i} -newcam -overclock
	fi
done
