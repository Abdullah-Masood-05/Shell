// Compress each to its own .7z
// A Nilesoft Shell Pro snippet. MIT license.
// Turn it on or off on the Snippets page of the app.
//
// Select several files or folders > Compress each to .7z. Every item gets
// its own archive next to it ("photos" becomes "photos.7z"), where 7-Zip's
// own menu would put them all in one. If an archive with that name exists,
// 7-Zip adds to it. Needs 7-Zip (64-bit, in Program Files).
item(where=path.exists(sys.prog + '\7-Zip\7z.exe') type='file|dir' mode='multiple' invoke='multiple'
	title='Compress each to .7z' image='@sys.prog\7-Zip\7zFM.exe' window=minimized
	cmd='@sys.prog\7-Zip\7z.exe' args=`a -t7z -mx=7 "@(sel.path).7z" "@sel.path"`)
