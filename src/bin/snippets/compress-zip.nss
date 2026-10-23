// Compress to ZIP
// A Nilesoft Shell Pro snippet. MIT license.
// Turn it on or off on the Snippets page of the app.
//
// Right-click files or folders > Compress to ZIP. The ZIP file goes next to
// them: one item becomes "<name>.zip", several become "<folder name>.zip".
// An existing ZIP is never overwritten: the new one gets " (2)", " (3)"...
// Uses PowerShell's Compress-Archive, which comes with Windows.
item(type='file|dir' mode='multiple' title='Compress to ZIP' image=icon.compressed window=hidden cmd='powershell.exe'
	args=`-NoProfile -NonInteractive -Command "$items = '@str.replace(sel(false, "|"), "'", "''")' -split '\|'; $base = '@str.replace(path.combine(sel.parent, if(sel.count == 1, sel.title, sel.parent.name)), "'", "''")'; $dest = $base + '.zip'; $n = 2; while (Test-Path -LiteralPath $dest) { $dest = $base + ' (' + $n + ').zip'; $n++ }; Compress-Archive -LiteralPath $items -DestinationPath $dest"`)
