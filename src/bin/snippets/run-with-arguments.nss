// Run with arguments
// A Nilesoft Shell Pro snippet. MIT license.
// Turn it on or off on the Snippets page of the app.
//
// Right-click a program (.exe) or a batch file (.bat, .cmd) > Run with
// arguments. A box asks for the arguments, then the program starts with
// them, in the folder it is in. Cancel the box to run nothing.
item(type='file' find='.exe|.bat|.cmd|.com' mode='single' title='Run with arguments' image=image.mdl(\uE768)
	cmd=if(input('Run with arguments', 'Arguments for ' + sel.name), shell(sel.path, input.result)))
