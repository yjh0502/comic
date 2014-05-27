#!/usr/bin/env sh

DIR="$1";
[ -z "$DIR" ] && DIR="./";

cd "${DIR}" || exit 1;
find -name "*.jpg" -or -name "*.JPG" | sort -V \
    | xargs -d "\n" comic;
