// New .gitignore
// A Nilesoft Shell Pro snippet. MIT license.
// Turn it on or off on the Snippets page of the app.
//
// Right-click the empty space in a folder > New .gitignore. Creates a
// .gitignore with common entries (system files, editor folders, build
// output, dependencies, logs). If the folder already has one, it is left
// as it is and a message says so.
item(type='back.dir' mode='single' title='New .gitignore' image=icon.new_file
	cmd=if(path.exists(path.combine(sel.curdir, '.gitignore')),
		msg('This folder already has a .gitignore.', app.name, msg.ok | msg.info),
		io.file.create(path.combine(sel.curdir, '.gitignore'),
			"# System files\nThumbs.db\ndesktop.ini\n.DS_Store\n\n# Editors\n.vs/\n.vscode/\n.idea/\n*.swp\n\n# Build output\nbin/\nobj/\nbuild/\ndist/\nout/\n\n# Dependencies\nnode_modules/\n.venv/\n__pycache__/\n\n# Logs and local settings\n*.log\n.env\n")))
