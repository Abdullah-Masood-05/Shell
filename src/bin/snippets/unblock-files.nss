// Unblock downloaded files
// A Nilesoft Shell Pro snippet. MIT license.
// Turn it on or off on the Snippets page of the app.
//
// Windows marks files that come from the internet, so they open in
// Protected View or ask before they run. "Unblock" removes that mark from
// the selected files (PowerShell's Unblock-File), the same as ticking
// Unblock in a file's Properties. On a folder it unblocks every file inside.
// Only do this for files you trust.
item(type='file' mode='multiple' invoke='multiple' title='Unblock' image=image.mdl(\uE785) window=hidden cmd='powershell.exe'
	args=`-NoProfile -NonInteractive -Command "Unblock-File -LiteralPath '@str.replace(sel.path, "'", "''")'"`)
item(type='dir' mode='multiple' invoke='multiple' title='Unblock files inside' image=image.mdl(\uE785) window=hidden cmd='powershell.exe'
	args=`-NoProfile -NonInteractive -Command "Get-ChildItem -LiteralPath '@str.replace(sel.path, "'", "''")' -Recurse -File | Unblock-File"`)
