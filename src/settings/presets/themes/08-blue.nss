// Preset: Blue
// Tags: Shell 1.x, Square
// About: Shell 1.x colours: white on deep blue, square corners.
// Source: Nilesoft Shell 1.x, templates/themes/blue.shl
// Ported from templates/themes/blue.shl (shell { default { ... } }, a format
// Shell 2 no longer reads; its screenshot is templates/themes/images/blue.png).
// The original values, one for one below:
//   symbol=#fff
//   text=[#fff, #fff, #004]      normal, highlighted, disabled
//   block=[#00b, #00f, 0]      background, highlight
//   separator=[#00f, 1]
//   frame=[#00f, 2]          border colour, size
//   shadow=false
//   //font = [12, 'Brush Script']   (commented out in the original too)
// 1.x menus were opaque with square corners, hence effect 0 and radius 0.
theme
{
	background
	{
		color=#00b
		opacity=100
		effect=0
	}
	item.text.normal=#fff
	item.text.select=#fff
	item.text.normal.disabled=#004
	item.back.select=#00f
	separator.color=#00f
	separator.size=1
	border.color=#00f
	border.size=2
	border.radius=0
	item.radius=0
	shadow.enabled=false
	symbol.normal=#fff
	symbol.select=#fff
}
