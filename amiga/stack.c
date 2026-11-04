/* Candy Crisis keeps its state in globals and recurses only a little
 * (the blob-group search), but a Shell's 4 KB is too little for its
 * dialogs' locals and the C library's stdio; libnix's startup sets this
 * much up instead.
 * The AmiCradle Team. GPL 2, like Candy Crisis. */
unsigned long __stack = 128 * 1024;
