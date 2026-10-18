// Copy file contents
// A Nilesoft Shell Pro snippet. MIT license.
// Turn it on or off on the Snippets page of the app.
//
// Right-click a text file (.txt, .md, .log, .json, code...) > Copy contents.
// The text of the file goes to the clipboard without opening it. Files over
// 1 MB are left out, so a huge log can't fill the clipboard.
item(type='file' mode='single' title='Copy contents' image=icon.copy
	find='.txt|.md|.log|.csv|.json|.xml|.yml|.yaml|.ini|.toml|.nss|.html|.css|.js|.ts|.py|.ps1|.bat|.cmd|.c|.cpp|.h|.cs|.java|.rs|.go|.sql'
	where=io.file.size(sel.path) < 1048576
	cmd=command.copy(io.file.read(sel.path)))
