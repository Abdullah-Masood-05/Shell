/*
	My Tools: New file and System menus

	By Abdullah Masood (@Abdullah-Masood-05), from
	https://github.com/Abdullah-Masood-05/nilesoft-shell-config
	(imports/my-tools.nss, MIT). Shipped here as part of the default
	configuration and imported last by shell.nss.

	Changes from the original:
	  - titles go through loc.* (English strings in imports/lang/en.nss)
	  - the Copy menu is left out: File manage > Copy path covers it
	  - empty entries (no extension, no parent) are hidden
	  - New file no longer truncates an existing file of the same name
	  - Restart Explorer uses the built-in command.restart_explorer
	  - Empty Recycle Bin uses the built-in command.empty_recycle_bin
	    (PowerShell's Clear-RecycleBin could fail without telling you)
	  - Empty Recycle Bin, Sign out, Restart and Shut down ask first

	Icons: icon.<name> come from imports/images.nss and are drawn as shapes, so
	they show on any PC (image.mdl(\uXXXX) needs the "Segoe MDL2 Assets" font,
	which some PCs lack); a #rrggbb value draws a colour chip.
*/


//----------------------- NEW FILE -----------------------------
// Created in the current folder. An existing file with the same name is left
// untouched (the original "type nul > name" would have emptied it).
menu(type='back|dir' title=loc.tools_new_file image=icon.new_file)
{
	item(title=loc.tools_new_txt dir=sel.curdir window=hidden
		cmd='cmd.exe' args='/c if not exist "new file.txt" (type nul > "new file.txt")')

	item(title=loc.tools_new_md dir=sel.curdir window=hidden
		cmd='cmd.exe' args='/c if not exist "new file.md" (type nul > "new file.md")')

	item(title=loc.tools_new_py dir=sel.curdir window=hidden
		cmd='cmd.exe' args='/c if not exist "new script.py" (type nul > "new script.py")')
}


//----------------------- SYSTEM / POWER -----------------------
// Desktop / folder background only.
menu(type='back|desktop' title=loc.tools_system image=icon.settings)
{

	item(title=loc.tools_restart_explorer image=icon.refresh
		cmd=command.restart_explorer)

	item(title=loc.tools_empty_recycle_bin image=icon.empty_recycle_bin
		cmd=if(msg(loc.tools_confirm_empty_recycle_bin, app.name, msg.yesno | msg.warning | msg.defbutton2) == msg.idyes,
			command.empty_recycle_bin))

	// Needs elevation
	item(title=loc.tools_flush_dns admin=true image=icon.sys_dns
		cmd='cmd.exe' window=hidden args='/c ipconfig /flushdns')

	// God Mode: every Control Panel task in one window
	item(title=loc.tools_god_mode image=icon.sys_god_mode
		cmd='explorer.exe' args='shell:::{ED7BA470-8E54-465E-825C-99712043E01C}')

	separator

	item(title=loc.tools_lock image=icon.sys_lock
		cmd='rundll32.exe' args='user32.dll,LockWorkStation')

	// Note: SetSuspendState hibernates instead when hibernation is enabled.
	item(title=loc.tools_sleep image=icon.sys_sleep
		cmd='rundll32.exe' args='powrprof.dll,SetSuspendState 0,1,0')

	item(title=loc.tools_sign_out image=icon.sys_sign_out
		cmd=if(msg(loc.tools_confirm_sign_out, app.name, msg.yesno | msg.warning | msg.defbutton2) == msg.idyes,
			shell('shutdown.exe', '/l', 'open')))

	item(title=loc.tools_restart image=icon.sys_restart
		cmd=if(msg(loc.tools_confirm_restart, app.name, msg.yesno | msg.warning | msg.defbutton2) == msg.idyes,
			shell('shutdown.exe', '/r /t 0', 'open')))

	item(title=loc.tools_shut_down image=icon.sys_power
		cmd=if(msg(loc.tools_confirm_shut_down, app.name, msg.yesno | msg.warning | msg.defbutton2) == msg.idyes,
			shell('shutdown.exe', '/s /t 0', 'open')))
}
