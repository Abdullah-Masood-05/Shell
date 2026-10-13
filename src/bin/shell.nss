settings
{
	priority=1
	exclude.where = !process.is_explorer

	// Submenus open 200 ms after hover while a Shell menu is open. This sets
	// SPI_SETMENUSHOWDELAY, overriding the Windows setting for that time; remove
	// the line (or turn it off in Nilesoft Shell Pro, Behavior) to use the Windows value.
	showdelay = 200

	// Options to allow modification of system items
	modify.remove.duplicate=1
	tip.enabled=true
}

// localization
// English is always loaded first so untranslated keys fall back to it, then
// the best match for sys.lang (e.g. "de-DE") is loaded on top of it:
//   exact locale (de-DE.nss) -> base language (de.nss) -> en.nss
// path.exists() resolves relative paths against the host process's current
// directory (not this file), so the path must be absolute: use app.dir.
$loc_path=app.dir + '\imports\lang\'
$loc_lang=if(str.contains(sys.lang, '-'), str.left(sys.lang, str.find(sys.lang, '-')), sys.lang)
// Norwegian: Windows reports nb-NO (Bokmal) or nn-NO (Nynorsk); the file is no.nss
$loc_base=if(loc_lang == 'nb' || loc_lang == 'nn', 'no', loc_lang)
import lang loc_path + "en.nss"
import lang if(path.exists(loc_path + sys.lang + ".nss"),
               loc_path + sys.lang + ".nss",
               if(path.exists(loc_path + loc_base + ".nss"),
                  loc_path + loc_base + ".nss",
                  loc_path + "en.nss"))

// or import lang 'imports/lang/en.nss'

import 'imports/theme.nss'
import 'imports/images.nss'
import 'imports/modify.nss'

menu(mode="multiple" title=loc.pin_unpin image=icon.pin)
{
}

menu(mode="multiple" title=title.more_options image=icon.more_options)
{
}

import 'imports/terminal.nss'
import 'imports/file-manage.nss'
import 'imports/develop.nss'
import 'imports/goto.nss'
import 'imports/taskbar.nss'

// Copy / New file / System menus, from Abdullah Masood's config
// (https://github.com/Abdullah-Masood-05/nilesoft-shell-config)
import 'imports/my-tools.nss'
