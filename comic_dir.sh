#!/usr/bin/env sh

DIR="$1";
[ -z "$DIR" ] && DIR="./";

cd "${DIR}" || exit 1;
MTREE_FILE=.mtree;
find ./ -name "*.jpg" -or -name "*.JPG" | sort \
    | xargs -d '\n' bsdtar -cf $MTREE_FILE \
        --format mtree --options '!all,type' \
    && comic $MTREE_FILE;
rm $MTREE_FILE;
