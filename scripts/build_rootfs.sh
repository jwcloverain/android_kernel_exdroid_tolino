#!/bin/bash

set -e

generate_rootfs()
{
    if [ -d skel ] ; then
    	(cd skel; find . | fakeroot cpio -o -Hnewc | gzip > ../"$1")
    else
    	echo "skel not exist"
    	exit 1
    fi
}

extract_rootfs()
{
	file_to_extract="${1}"
	file_name="$(basename "${file_to_extract}")"
	cd "$(dirname "${file_to_extract}")"
	file_path_abs="$(pwd)"
	cd -

	if [ -f "${file_to_extract}" ] ; then
		rm -rf skel && mkdir skel
		if [ "$(file -z "${file_to_extract}"|grep "tar archive")" ];then
			(cd skel; fakeroot tar xvf "${file_path_abs}/${file_name}" ./)
		else
			gzip -dc "${file_to_extract}" | (cd skel; fakeroot cpio -i)
		fi
	else
		echo "\"${file_to_extract}\" not exist"
		exit 1
	fi
}

if [ $# -ne 2 ]; then
	echo -e "please input correct parameters"
	echo -e "\t[build.sh e rootf.cpio.gz] to extract the rootfs template to skel folder"
	echo -e "\tthen make some changes in the skel folder"
	echo -e "\t[build.sh c rootf.cpio.gz] to create the rootfs from the skel folder"
	exit 1
fi

if [ "$1" = "e" ] ; then
	extract_rootfs $2
elif [ "$1" = "c" ] ; then
	generate_rootfs $2
else
	echo "Wrong arguments"
	exit 1
fi
