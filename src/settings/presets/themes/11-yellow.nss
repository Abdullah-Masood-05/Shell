// Preset: Yellow
// Tags: Shell 1.x, Square
// About: Shell 1.x colours: white on olive, yellow highlight.
// Source: Nilesoft Shell 1.x, templates/themes/yellow.shl
// Ported from templates/themes/yellow.shl (shell { default { ... } }, a format
// Shell 2 no longer reads; its screenshot is templates/themes/images/yellow.png).
// The original values, one for one below:
//   symbol=#fff
//   text=[#fff, #888, #660]      normal, highlighted, disabled
//   block=[#bb0, #ff0, 0]      background, highlight
//   separator=[#ff0, 1]
//   frame=[#ff0, 2]          border colour, size
//   shadow=false
//   //font = [12, 'Brush Script']   (commented out in the original too)
// 1.x menus were opaque with square corners, hence effect 0 and radius 0.
theme
{
	background
	{
		color=#bb0
		opacity=100
		effect=0
	}
	item.text.normal=#fff
	item.text.select=#888
	item.text.normal.disabled=#660
	item.back.select=#ff0
	separator.color=#ff0
	separator.size=1
	border.color=#ff0
	border.size=2
	border.radius=0
	item.radius=0
	shadow.enabled=false
	symbol.normal=#fff
	symbol.select=#fff
}
