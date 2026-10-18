// Extract ZIP to a folder
// A Nilesoft Shell Pro snippet. MIT license.
// Turn it on or off on the Snippets page of the app.
//
// Right-click a .zip file > Extract to folder. Everything inside goes into a
// new folder next to the ZIP, named after it, in one click (Windows' own
// "Extract All" asks where first). An existing folder is never mixed into:
// the new one gets " (2)", " (3)"... Uses PowerShell's Expand-Archive.
item(type='file' find='.zip' mode='multiple' invoke='multiple' title='Extract to folder' image=icon.extract_all window=hidden cmd='powershell.exe'
	args=`-NoProfile -NonInteractive -Command "$base = '@str.replace(path.combine(sel.parent, sel.title), "'", "''")'; $dest = $base; $n = 2; while (Test-Path -LiteralPath $dest) { $dest = $base + ' (' + $n + ')'; $n++ }; Expand-Archive -LiteralPath '@str.replace(sel.path, "'", "''")' -DestinationPath $dest"`)
