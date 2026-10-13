/*
	Win+X: a power-user menu for Win+X and right-click on Start

	EXPERIMENTAL. Not imported by default: uncomment the
	`import 'imports/winx.nss'` line at the end of shell.nss to use it.

	It replaces Windows' own Win+X items (settings.winx.replace below). Those
	items are not read at all while it is on, so Explorer never receives a
	command it did not put in the menu. Remove the settings block to add these
	items next to Windows' own instead.

	Access keys: press Win+X, then the underlined letter (&X in a title marks
	X). Each letter is used once on this level, and not I or G: the shipped
	Terminal and Go To menus (terminal.nss, goto.nss) show here too and use
	them. Where it can, a letter matches Windows' own Win+X menu.

	To see how a menu was recognised, set settings.winx.debug = true and look
	for "Win+X" in shell.log (see the Win+X page of the documentation).

	Icons are "Segoe MDL2 Assets" glyphs (image.mdl), as in my-tools.nss.
*/

settings
{
	winx.replace = true
}

item(type='winx' title='Termi&nal' image=image.mdl(\uE756)
	cmd=if(package.exists("WindowsTerminal"), 'wt.exe', 'powershell.exe'))

item(type='winx' title='Terminal (&Admin)' admin=true image=image.mdl(\uE7EF)
	cmd=if(package.exists("WindowsTerminal"), 'wt.exe', 'powershell.exe'))

separator(type='winx')

item(type='winx' title='Device &Manager' image=image.mdl(\uE772) cmd='devmgmt.msc')
item(type='winx' title='Dis&k Management' image=image.mdl(\uEDA2) cmd='diskmgmt.msc')
item(type='winx' title='&Event Viewer' image=image.mdl(\uE81C) cmd='eventvwr.msc')
item(type='winx' title='Ser&vices' image=image.mdl(\uE912) cmd='services.msc')
item(type='winx' title='&Task Manager' image=image.mdl(\uE9D9) cmd='taskmgr.exe')

separator(type='winx')

item(type='winx' title='&Settings' image=image.mdl(\uE713) cmd='ms-settings:')
item(type='winx' title='Net&work' image=image.mdl(\uE968) cmd='ms-settings:network')
item(type='winx' title='Power &Options' image=image.mdl(\uE945) cmd='ms-settings:powersleep')
item(type='winx' title='&Run' image=image.mdl(\uE8A7) cmd=command.run)

separator(type='winx')

// Each asks first; No (the default button) does nothing.
menu(type='winx' title='Sh&ut down or sign out' image=image.mdl(\uE7E8))
{
	item(title='&Lock' image=image.mdl(\uE72E)
		cmd='rundll32.exe' args='user32.dll,LockWorkStation')

	item(title='Sign &out' image=image.mdl(\uE748)
		cmd=if(msg('Sign out now?', app.name, msg.yesno | msg.warning | msg.defbutton2) == msg.idyes,
			shell('shutdown.exe', '/l', 'open')))

	separator

	item(title='&Restart' image=image.mdl(\uE777)
		cmd=if(msg('Restart now?', app.name, msg.yesno | msg.warning | msg.defbutton2) == msg.idyes,
			shell('shutdown.exe', '/r /t 0', 'open')))

	item(title='Sh&ut down' image=image.mdl(\uE7E8)
		cmd=if(msg('Shut down now?', app.name, msg.yesno | msg.warning | msg.defbutton2) == msg.idyes,
			shell('shutdown.exe', '/s /t 0', 'open')))
}
