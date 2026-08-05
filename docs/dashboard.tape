# vhs script for the dashboard GIF.  Record with:  vhs docs/dashboard.tape
# Install vhs: https://github.com/charmbracelet/vhs
Output docs/dashboard.gif
Set FontSize 13
Set Width 1400
Set Height 800
Set Theme "Catppuccin Mocha"
Set TypingSpeed 40ms

Type "./build/cli/canforge dash --config sim/tests/data/vehicle.cfg --virtual"
Enter
Sleep 4s

# 2: the grouped view, the one engineers actually watch
Type "2"
Sleep 5s

# 3: the signal plot
Type "3"
Sleep 1s
Type "s"
Sleep 500ms
Type "EngineData.EngineSpeed"
Enter
Sleep 6s

# 4: bus statistics and the per-node breakdown
Type "4"
Sleep 4s

# 1: back to the decoded trace, then pause it
Type "1"
Sleep 3s
Space
Sleep 2s
Space

# the help overlay
Type "?"
Sleep 3s
Type "?"
Sleep 1s
Type "q"
Sleep 1s
