// Preset: Gray
// Tags: Shell 1.x, Square
// About: Shell 1.x colours: white on grey, light highlight.
// Source: Nilesoft Shell 1.x, templates/themes/gray.shl
// Ported from templates/themes/gray.shl (shell { default { ... } }, a format
// Shell 2 no longer reads; its screenshot is templates/themes/images/gray.png).
// The original values, one for one below:
//   symbol=#fff
//   text=[#fff, #aaa, #555]      normal, highlighted, disabled
//   block=[#888, #eee, 0]      background, highlight
//   separator=[#fff, 1]
//   frame=[#fff, 2]          border colour, size
//   shadow=false
// 1.x menus were opaque with square corners, hence effect 0 and radius 0.
theme
{
	background
	{
		color=#888
		opacity=100
		effect=0
	}
	item.text.normal=#fff
	item.text.select=#aaa
	item.text.normal.disabled=#555
	item.back.select=#eee
	separator.color=#fff
	separator.size=1
	border.color=#fff
	border.size=2
	border.radius=0
	item.radius=0
	shadow.enabled=false
	symbol.normal=#fff
	symbol.select=#fff
}
