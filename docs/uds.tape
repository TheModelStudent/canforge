# vhs script for the UDS firmware download GIF.  Record with:  vhs docs/uds.tape
Output docs/uds.gif
Set FontSize 14
Set Width 1100
Set Height 650
Set Theme "Catppuccin Mocha"
Set TypingSpeed 45ms

Type "head -c 2048 /dev/urandom > firmware.bin"
Enter
Sleep 1s
Type "./build/cli/canforge uds --virtual --download firmware.bin"
Enter
Sleep 8s
