// Preset: Green
// Tags: Shell 1.x, Square
// About: Shell 1.x colours: black on green, square corners.
// Source: Nilesoft Shell 1.x, templates/themes/green.shl
// Ported from templates/themes/green.shl (shell { default { ... } }, a format
// Shell 2 no longer reads; its screenshot is templates/themes/images/green.png).
// The original values, one for one below:
//   symbol=#000
//   text=[#000, #000, #040]      normal, highlighted, disabled
//   block=[#0b0, #0f0, 0]      background, highlight
//   separator=[#0f0, 1]
//   frame=[#0f0, 2]          border colour, size
//   shadow=false
//   //font = [12, 'Brush Script']   (commented out in the original too)
// 1.x menus were opaque with square corners, hence effect 0 and radius 0.
theme
{
	background
	{
		color=#0b0
		opacity=100
		effect=0
	}
	item.text.normal=#000
	item.text.select=#000
	item.text.normal.disabled=#040
	item.back.select=#0f0
	separator.color=#0f0
	separator.size=1
	border.color=#0f0
	border.size=2
	border.radius=0
	item.radius=0
	shadow.enabled=false
	symbol.normal=#000
	symbol.select=#000
}
