====================
Duktape command line
====================

Ecmascript plugin loading execution tool, useful for running Ecmascript code
from files. Based on the command line tool.

Simply run duk ./whatever.so whatever.js

Plugins are loaded from the left to the right.

Plugins are loaded using dlopen, so if only a filename is given, LD_LIBRARY_PATH is searched.