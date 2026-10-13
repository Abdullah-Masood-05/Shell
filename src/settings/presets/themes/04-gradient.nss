// Preset: Gradient
// Tags: Gradient, Accent
// About: An accent gradient from dark to light and back (README screenshot).
// Source: moudey, Nilesoft Shell discussion #174
// https://github.com/moudey/Shell/discussions/174, the "Gradient" block, which the
// maintainer says to add to the theme section. screenshots/gradient.png shows it.
theme
{
    background
    {
        opacity=0
        gradient
        {
            enabled = 1
            linear = [0, 100, 0, 0]
            stop = [
                [0, color.accent_dark3, 90],
                [.5, color.accent, 90],
                [1, color.accent_dark3, 90]
            ]
        }
    }
}
