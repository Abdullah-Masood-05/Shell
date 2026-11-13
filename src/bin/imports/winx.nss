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

*/

settings
{
	winx.replace = true
	// Win+X and right-click on Start open this menu on Windows 11 builds whose own
	// Win+X menu Shell cannot fill (see the Win+X page of the documentation)
	winx.hotkey = true
}

// Icons: the two-tone set of imports/images.nss (icon.*) where it has one, and
// these, drawn the same way (image.color1 lines, image.color2 accent), where
// it does not. Without imports/images.nss the icon.* ones are left out.
@winx_event = '<svg fill="none" stroke-linecap="round" stroke-linejoin="round" viewBox="0 0 16 16"><rect x="2.5" y="1.5" width="11" height="13" rx="2" stroke="@image.color1"/><path stroke="@image.color2" d="M5 5h6M5 8h6M5 11h3"/></svg>'
@winx_services = '<svg fill="none" stroke-linecap="round" stroke-linejoin="round" viewBox="0 0 16 16"><circle cx="8" cy="8" r="4.5" stroke="@image.color1"/><path stroke="@image.color1" d="M8 1.5v2M8 12.5v2M1.5 8h2M12.5 8h2M3.4 3.4l1.4 1.4M11.2 11.2l1.4 1.4M3.4 12.6l1.4-1.4M11.2 4.8l1.4-1.4"/><circle cx="8" cy="8" r="2" stroke="@image.color2"/></svg>'
@winx_network = '<svg fill="none" stroke-linecap="round" stroke-linejoin="round" viewBox="0 0 16 16"><rect x="1.5" y="2.5" width="13" height="9" rx="1.5" stroke="@image.color1"/><path stroke="@image.color1" d="M5.5 14.5h5M8 11.5v3"/><path stroke="@image.color2" d="M5.2 7.2a4 4 0 0 1 5.6 0M6.8 8.8a1.7 1.7 0 0 1 2.4 0"/></svg>'
@winx_power = '<svg fill="none" stroke-linecap="round" stroke-linejoin="round" viewBox="0 0 16 16"><rect x="1.5" y="4.5" width="11.5" height="7" rx="1.5" stroke="@image.color1"/><path stroke="@image.color1" d="M14.8 7v2"/><rect x="3.2" y="6.2" width="5.5" height="3.6" rx=".6" fill="@image.color2"/></svg>'
@winx_run = '<svg fill="none" stroke-linecap="round" stroke-linejoin="round" viewBox="0 0 16 16"><rect x="1.5" y="2.5" width="13" height="11" rx="2" stroke="@image.color1"/><path fill="@image.color2" stroke="@image.color2" d="M6.5 6l3.5 2-3.5 2z"/></svg>'
@winx_shutdown = '<svg fill="none" stroke-linecap="round" stroke-linejoin="round" viewBox="0 0 16 16"><path stroke="@image.color1" d="M4.6 3.9a5.5 5.5 0 1 0 6.8 0"/><path stroke="@image.color2" d="M8 1.5v6"/></svg>'
@winx_lock = '<svg fill="none" stroke-linecap="round" stroke-linejoin="round" viewBox="0 0 16 16"><path stroke="@image.color1" d="M5 7V5a3 3 0 0 1 6 0v2"/><rect x="3.5" y="7" width="9" height="7" rx="1.5" fill="@image.color2"/></svg>'
@winx_signout = '<svg fill="none" stroke-linecap="round" stroke-linejoin="round" viewBox="0 0 16 16"><path stroke="@image.color1" d="M9.5 4V2.5a1 1 0 0 0-1-1h-5a1 1 0 0 0-1 1v11a1 1 0 0 0 1 1h5a1 1 0 0 0 1-1V12"/><path stroke="@image.color2" d="M6.5 8h8M12 5.5L14.5 8 12 10.5"/></svg>'
@winx_restart = '<svg fill="none" stroke-linecap="round" stroke-linejoin="round" viewBox="0 0 16 16"><path stroke="@image.color1" d="M13.5 8a5.5 5.5 0 1 1-1.6-3.9"/><path stroke="@image.color2" d="M12.5 1.5v3h-3"/></svg>'

item(type='winx' title='Termi&nal' image=icon.command_prompt
	cmd=if(package.exists("WindowsTerminal"), 'wt.exe', 'powershell.exe'))

item(type='winx' title='Terminal (&Admin)' admin=true image=icon.command_prompt_admin
	cmd=if(package.exists("WindowsTerminal"), 'wt.exe', 'powershell.exe'))

separator(type='winx')

item(type='winx' title='Device &Manager' image=icon.device_manager cmd='devmgmt.msc')
item(type='winx' title='Dis&k Management' image=icon.disk_management cmd='diskmgmt.msc')
item(type='winx' title='&Event Viewer' image=icon.winx_event cmd='eventvwr.msc')
item(type='winx' title='Ser&vices' image=icon.winx_services cmd='services.msc')
item(type='winx' title='&Task Manager' image=icon.task_manager cmd='taskmgr.exe')

separator(type='winx')

item(type='winx' title='&Settings' image=icon.settings cmd='ms-settings:')
item(type='winx' title='Net&work' image=icon.winx_network cmd='ms-settings:network')
item(type='winx' title='Power &Options' image=icon.winx_power cmd='ms-settings:powersleep')
item(type='winx' title='&Run' image=icon.winx_run cmd=command.run)

separator(type='winx')

// Each asks first; No (the default button) does nothing.
menu(type='winx' title='Sh&ut down or sign out' image=icon.winx_shutdown)
{
	item(title='&Lock' image=icon.winx_lock
		cmd='rundll32.exe' args='user32.dll,LockWorkStation')

	item(title='Sign &out' image=icon.winx_signout
		cmd=if(msg('Sign out now?', app.name, msg.yesno | msg.warning | msg.defbutton2) == msg.idyes,
			shell('shutdown.exe', '/l', 'open')))

	separator

	item(title='&Restart' image=icon.winx_restart
		cmd=if(msg('Restart now?', app.name, msg.yesno | msg.warning | msg.defbutton2) == msg.idyes,
			shell('shutdown.exe', '/r /t 0', 'open')))

	item(title='Sh&ut down' image=icon.winx_shutdown
		cmd=if(msg('Shut down now?', app.name, msg.yesno | msg.warning | msg.defbutton2) == msg.idyes,
			shell('shutdown.exe', '/s /t 0', 'open')))
}
