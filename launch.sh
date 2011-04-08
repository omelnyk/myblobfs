#!/bin/bash
val=$RANDOM
echo "Using value $val"
tmpdir="/tmp/fuse$val/"
mkdir $tmpdir
./src/myblobfs --database=virtualdir --table=test --name-field=id --data-field=content --user=root -p $tmpdir
ls $tmpdir
