// Copy folder tree
// A Nilesoft Shell Pro snippet. MIT license.
// Turn it on or off on the Snippets page of the app.
//
// Right-click a folder (or the empty space inside one) > Copy folder tree.
// The folders and files under it, drawn as a tree with Windows' "tree /f /a"
// command, go to the clipboard: handy for notes, chats and bug reports.
item(type='dir|back.dir' mode='single' title='Copy folder tree' image=image.mdl(\uE8B7) window=hidden cmd='powershell.exe'
	args=`-NoProfile -NonInteractive -Command "tree.com '@str.replace(if(sel.back, sel.curdir, sel.path), "'", "''")' /f /a | Set-Clipboard"`)
