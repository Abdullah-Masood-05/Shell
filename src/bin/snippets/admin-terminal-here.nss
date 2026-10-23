// Open here as administrator
// A Nilesoft Shell Pro snippet. MIT license.
// Turn it on or off on the Snippets page of the app.
//
// Right-click a folder, a drive or the empty space in a folder > Open here
// as administrator > Command Prompt or PowerShell. Windows asks for
// permission (UAC) first; the window starts in that folder. Commands typed
// there can change anything on the PC, so close it when you are done.
menu(type='dir|back.dir|drive|back.drive' mode='single' title='Open here as administrator' image=icon.run_as_administrator)
{
	item(title='Command Prompt' admin image=icon.command_prompt cmd='cmd.exe'
		args=`/k pushd "@sel.dir"`)
	item(title='PowerShell' admin image=icon.windows_powershell cmd='powershell.exe'
		args=`-NoExit -NoProfile -Command "Set-Location -LiteralPath '@str.replace(sel.dir, "'", "''")'"`)
}
