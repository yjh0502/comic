comic
=====

Minimalistic image viewer for Linux

# Features
 - Display jpeg-encoded images
 - Display multiple images and archived images, for example, images in .zip file
 - Keyboard based navigation

# Compile and install

`comic` uses following libraries.

 - `Xlib` for X11
 - `libjpeg` or `libjpeg-turbo` to decode jpeg images
 - (optional) `libarchive` to read archived images

Use `make` to compile, `make install` to install. Please refer `config.mk` to tune compile options. Use `make ARCHIVE_SUPPORT=1` if you have libarchive and want to read archived images.

# Run

```sh
comic archive.zip # show images in archive.zip file
comic *.jpg # show all images in current directory
comic_dir.sh ./ # show all images in directory, recursively
```

# Customize

All keyboard shortcuts are defined in `config.h` file, so edit it and recompile to customize keyboard shortcuts.

# Known bugs

 - There is an known bug that libarchive cannot read some zip files. The bug was fixed in master branch of libarchive. It it bothers you, compile & install libarchive master branch, and recompile comic. If you don't want to compile libarchive, you can fix the problem by re-archiving zip file with `unzip` tool.
