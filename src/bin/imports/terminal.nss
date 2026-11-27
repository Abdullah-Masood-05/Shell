// Not on a library (Documents, Pictures, ... under Libraries): it is a
// .library-ms file, not a folder, so a terminal cannot start in it.
menu(type='*' where=(sel.count or wnd.is_taskbar or wnd.is_edit) and !(str.end(sel.raw, '.library-ms') or str.end(sel.path, '.library-ms'))
	title=title.terminal sep=sep.top image=icon.run_with_powershell)
{
	$tip_run_admin=["\xE1A7 Press [SHIFT key] or [RIGHT-CLICK] to run " + this.title + " as administrator", tip.warning, 1.0]
	$has_admin=key.shift() or key.rbutton()
	
	item(title=title.command_prompt tip=tip_run_admin admin=has_admin image cmd-prompt=`/K TITLE Command Prompt &ver& PUSHD "@sel.dir"`)
	item(title=title.windows_powershell admin=has_admin tip=tip_run_admin image cmd-ps=`-noexit -command Set-Location -Path '@sel.dir'`)
	// PowerShell 7 (pwsh.exe), only when it is installed: the MSI one under Program Files, else
	// the Microsoft Store one, started through its pwsh.exe alias (the package folder itself
	// cannot be run from). The Store's own pwsh.exe is only used for the icon.
	// ProgramW6432 is the 64-bit Program Files even in a 32-bit program, where sys.prog is
	// Program Files (x86); it is not set on 32-bit Windows, so sys.prog is used there.
	$prog64 = sys.var('ProgramW6432')
	$pwsh_msi = if(length(prog64) > 0, prog64, sys.prog) + '\PowerShell\7\pwsh.exe'
	$pwsh_store = user.localappdata + '\Microsoft\WindowsApps\pwsh.exe'
	$pwsh = if(path.exists(pwsh_msi), pwsh_msi, if(package.exists("Microsoft.PowerShell_") and path.exists(pwsh_store), pwsh_store, ''))
	$pwsh_icon = if(pwsh == pwsh_store, package.path("Microsoft.PowerShell_") + '\pwsh.exe', pwsh)
	item(where=length(pwsh) > 0
			title='PowerShell 7'
			tip=tip_run_admin
			admin=has_admin
			image=pwsh_icon
			cmd=pwsh
			arg=`-NoExit -WorkingDirectory "@sel.dir\."`)
	item(where=package.exists("WindowsTerminal")
			title=title.Windows_Terminal
			tip=tip_run_admin
			admin=has_admin
			image='@package.path("WindowsTerminal")\WindowsTerminal.exe'
			cmd="wt.exe"
			arg=`-d "@sel.path\."`)
}