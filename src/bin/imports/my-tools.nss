/*
	My Tools: Copy, New file and System menus

	By Abdullah Masood (@Abdullah-Masood-05), from
	https://github.com/Abdullah-Masood-05/nilesoft-shell-config
	(imports/my-tools.nss, MIT). Shipped here as part of the default
	configuration and imported last by shell.nss.

	Changes from the original:
	  - titles go through loc.* (English strings in imports/lang/en.nss)
	  - Copy is mode="multiple", so "Copy N paths" shows for a multi-selection
	  - empty entries (no extension, no parent) are hidden
	  - New file no longer truncates an existing file of the same name
	  - Restart Explorer uses the built-in command.restart_explorer
	  - Empty Recycle Bin, Sign out, Restart and Shut down ask first

	Icons: icon.<name> come from imports/images.nss; image.mdl(\uXXXX) is a
	"Segoe MDL2 Assets" glyph; a #rrggbb value draws a colour chip.
*/


//---------------------------- COPY ----------------------------
menu(mode="multiple" type='file|dir|back' title=loc.tools_copy image=icon.copy)
{
	// Multi-select: every selected path, one per line
	item(where=sel.count > 1 title=loc.tools_copy_paths
		cmd=command.copy(sel(false, "\n")))

	// Full path
	item(mode="single" title=sel.path
		cmd=command.copy(sel.path))

	// Quoted path ("C:\...") ready to paste into a terminal
	item(mode="single" title=sel.path.quote
		cmd=command.copy(sel.path.quote))

	separator

	// Name with extension
	item(mode="single" type='file|dir' title=sel.name
		cmd=command.copy(sel.name))

	// Name without extension
	item(mode="single" type='file' where=sel.file.ext.len > 0 title=sel.title
		cmd=command.copy(sel.title))

	// Extension only
	item(mode="single" type='file' where=sel.file.ext.len > 0 title=sel.file.ext
		cmd=command.copy(sel.file.ext))

	separator

	// Parent folder path
	item(mode="single" where=sel.parent.len > 0 title=sel.parent
		cmd=command.copy(sel.parent))
}


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

	item(title=loc.tools_restart_explorer image=image.mdl(\uE72C)
		cmd=command.restart_explorer)

	item(title=loc.tools_empty_recycle_bin image=image.mdl(\uE74D)
		cmd=if(msg(loc.tools_confirm_empty_recycle_bin, app.name, msg.yesno | msg.warning | msg.defbutton2) == msg.idyes,
			shell('powershell.exe', '-NoProfile -WindowStyle Hidden -Command Clear-RecycleBin -Force -ErrorAction SilentlyContinue', 'open')))

	// Needs elevation
	item(title=loc.tools_flush_dns admin=true image=#20bf6b
		cmd='cmd.exe' window=hidden args='/c ipconfig /flushdns')

	// God Mode: every Control Panel task in one window
	item(title=loc.tools_god_mode image=#9b59b6
		cmd='explorer.exe' args='shell:::{ED7BA470-8E54-465E-825C-99712043E01C}')

	separator

	item(title=loc.tools_lock image=image.mdl(\uE72E)
		cmd='rundll32.exe' args='user32.dll,LockWorkStation')

	// Note: SetSuspendState hibernates instead when hibernation is enabled.
	item(title=loc.tools_sleep image=#34495e
		cmd='rundll32.exe' args='powrprof.dll,SetSuspendState 0,1,0')

	item(title=loc.tools_sign_out image=#e67e22
		cmd=if(msg(loc.tools_confirm_sign_out, app.name, msg.yesno | msg.warning | msg.defbutton2) == msg.idyes,
			shell('shutdown.exe', '/l', 'open')))

	item(title=loc.tools_restart image=#e74c3c
		cmd=if(msg(loc.tools_confirm_restart, app.name, msg.yesno | msg.warning | msg.defbutton2) == msg.idyes,
			shell('shutdown.exe', '/r /t 0', 'open')))

	item(title=loc.tools_shut_down image=#c0392b
		cmd=if(msg(loc.tools_confirm_shut_down, app.name, msg.yesno | msg.warning | msg.defbutton2) == msg.idyes,
			shell('shutdown.exe', '/s /t 0', 'open')))
}
