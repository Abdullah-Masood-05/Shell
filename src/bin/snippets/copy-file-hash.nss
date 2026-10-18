// Copy file hash
// A Nilesoft Shell Pro snippet. MIT license.
// Turn it on or off on the Snippets page of the app.
//
// Right-click a file > Copy hash > SHA-256, SHA-1 or MD5. PowerShell's
// Get-FileHash works it out and puts it on the clipboard, in lower case.
// Nothing leaves your PC.
menu(type='file' mode='single' title='Copy hash' image=icon.copy)
{
	item(title='SHA-256' window=hidden cmd='powershell.exe'
		args=`-NoProfile -NonInteractive -Command "(Get-FileHash -LiteralPath '@str.replace(sel.path, "'", "''")' -Algorithm SHA256).Hash.ToLower() | Set-Clipboard"`)
	item(title='SHA-1' window=hidden cmd='powershell.exe'
		args=`-NoProfile -NonInteractive -Command "(Get-FileHash -LiteralPath '@str.replace(sel.path, "'", "''")' -Algorithm SHA1).Hash.ToLower() | Set-Clipboard"`)
	item(title='MD5' window=hidden cmd='powershell.exe'
		args=`-NoProfile -NonInteractive -Command "(Get-FileHash -LiteralPath '@str.replace(sel.path, "'", "''")' -Algorithm MD5).Hash.ToLower() | Set-Clipboard"`)
}
