#/bin/bash
if [ -d output/images/rootfs ]; then
	rm -rf output/images/rootfs
fi
mkdir output/images/rootfs
rm -rf output/images/rootfs.tar
tar -vxjf output/images/rootfs.tar.bz2 -C output/images/rootfs

GCC="output/host/bin/riscv64-buildroot-linux-gnu-gcc"

for file in `ls post_sbin`
    do
        filePath="post_sbin/"$file
        if [ -f $filePath ]; then
            gccout="output/images/rootfs"${file%".c"}
            if [ -f $gccout ]; then
                rm $gccout
            fi
            $GCC $filePath -o $gccout -O2
        fi
    done
tar -vcjf output/images/rootfs.tar -C output/images/rootfs/*
