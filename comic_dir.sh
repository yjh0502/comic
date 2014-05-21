#!/usr/bin/env sh

DIR=$1
[ ! -n $DIR ] || DIR="./"

cd $DIR || exit 1
MTREE_FILE=.mtree
find ./ -name "*.jpg" -or -name "*.JPG" | sort \
    | xargs bsdtar -zcf $MTREE_FILE --format mtree \
    && comic $MTREE_FILE
rm $MTREE_FILE
