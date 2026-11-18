// Quick tools
// A Nilesoft Shell Pro snippet. MIT license.
// Turn it on or off on the Snippets page of the app.
//
// Copy details about this PC, bookmark a folder and come back to it from
// any right-click, and copy a path in the format another program wants.
// The bookmark is kept for this Windows account (app.set) until you clear it.

menu(title='Quick tools' image=image.mdl(\uE945))
{
	menu(title='PC info (click to copy)' image=image.mdl(\uE946))
	{
		item(title=str.fmt('Screen: {0} x {1}', sys.screen.width, sys.screen.height)
			cmd=clipboard.set(str.fmt('{0} x {1}', sys.screen.width, sys.screen.height)))
		item(title=str.fmt('Screens: {0}', sys.screen.count) cmd=clipboard.set(sys.screen.count))
		item(title=str.fmt('Memory: {0} MB free of {1} MB', sys.memory.free, sys.memory.total)
			cmd=clipboard.set(str.fmt('{0} MB free of {1} MB', sys.memory.free, sys.memory.total)))
		item(title=str.fmt('Processors: {0}', sys.cpu.count) cmd=clipboard.set(sys.cpu.count))
		item(title=str.fmt('Battery: {0}%{1}', sys.power.battery, if(sys.power.is_charging, ', charging', ''))
			where=sys.power.battery >= 0 cmd=clipboard.set(str.fmt('{0}%', sys.power.battery)))
		item(title=str.fmt('Language: {0}', sys.lang.full) cmd=clipboard.set(sys.lang.full))
	}

	separator

	item(title='Bookmark this folder' image=image.mdl(\uE718)
		cmd=app.set('quick-tools-bookmark', if(sel.type == type.dir, sel.path, sel.dir)))
	item(title='Open bookmark: ' + app.get('quick-tools-bookmark') image=image.mdl(\uE8B7)
		where=app.exists('quick-tools-bookmark') cmd=app.get('quick-tools-bookmark'))
	item(title='Clear bookmark' where=app.exists('quick-tools-bookmark')
		cmd=app.remove('quick-tools-bookmark'))

	separator

	item(title='Copy path as...' image=image.mdl(\uE8C8) where=sel.count > 0
		cmd=if(input('Copy path as', 'Format', '', ['Windows', 'Forward slashes', 'In quotes', 'File URL'], 0),
			clipboard.set(
				if(input.selected == 1, str.replace(sel.path, '\', '/'),
				if(input.selected == 2, '"' + sel.path + '"',
				if(input.selected == 3, 'file:///' + str.replace(sel.path, '\', '/'),
				sel.path))))))
}
