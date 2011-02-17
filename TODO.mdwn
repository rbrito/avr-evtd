# `bite sized` tasks to make the code better.

* Move many (all) of the conditional compilation of the preprocessor to a
  header file (akin to the Linux kernel practice).

  This will unclutter the code *a lot*.
* Get rid of the Hungarian notation in variables.
* Split functions that are too long with as many functions that do one thing
  and to that well.
* Move all those hard-to-read hex codes passed to the write_uart function to
  an enum so that we can some type safety (performed at compile time *and*
  lebility, without increasing the memory requirements of the probram.
* Get rid of legacy CVS/SVN tags in the code and put those things
  manually.
* Reformat the changelog that is shipped with the sources, so that it reads
  like a real GNU-style changelog.
* Unpack stuff from the `Install/recovery.tar` tarball and generate the
  tarball *only* during buildtime.
* Test that the emergency scripts actually work (read: audit the souce code,
  imagine a disaster recovery situation happening, and see if there is any
  room for improvements).
* Improve the manpage avr-evtd.8 both ortographically (spell-checking),
  grammatically, semantially, and typographically.
* No need to have the `COPYING` file twice in the installed package, as long
  as the files have all the Copyright assignment.
* Merge the `README*` files, remove outdated comment and fill in with newer
  content.
* Look for FIXMEs in the code and fix them.
