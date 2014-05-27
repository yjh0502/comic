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
 - `libarchive` to read archived images

Use `make` to compile, `make install` to install. Please refer `config.mk` to tune compile options.

# Run

```sh
comic archive.zip # show images in archive.zip file
comic *.jpg # show all images in current directory
comic_dir.sh ./ # show all images in directory, recursively
```

# Customize

All keyboard shortcuts are defined in `config.h` file, so edit it and recompile to customize keyboard shortcuts.
