// Look up on VirusTotal
// A Nilesoft Shell Pro snippet. MIT license.
// Turn it on or off on the Snippets page of the app.
//
// Right-click a file > Look up on VirusTotal. The file's SHA-256 hash is
// worked out on your PC and the VirusTotal report for that hash opens in
// your browser. Only the hash is sent, never the file itself. If VirusTotal
// has never seen the file, the page says so.
item(type='file' mode='single' title='Look up on VirusTotal' image=image.mdl(\uE721) window=hidden cmd='powershell.exe'
	args=`-NoProfile -NonInteractive -Command "Start-Process ('https://www.virustotal.com/gui/file/' + (Get-FileHash -LiteralPath '@str.replace(sel.path, "'", "''")' -Algorithm SHA256).Hash)"`)
