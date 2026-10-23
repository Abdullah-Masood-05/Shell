// Open in a new Windows Terminal tab
// A Nilesoft Shell Pro snippet. MIT license.
// Turn it on or off on the Snippets page of the app.
//
// Right-click a folder, a drive or the empty space in a folder > Open in a
// new Terminal tab. The folder opens as a new tab in the Windows Terminal
// window you used last, instead of a new window. Needs Windows Terminal.
item(where=package.exists("WindowsTerminal") type='dir|back.dir|drive|back.drive' mode='single'
	title='Open in a new Terminal tab' image=icon.open_new_tab cmd='wt.exe'
	args=`-w 0 nt -d "@sel.path\."`)
