# OpenINET project
open source firmware to turn your esp32 into usb wifi dongle

# purpose
i was too broke to get a real network card for my freebsd machine and the wifi dongles were sloppy, propreitary, and could break for no reason
esp32 on the other hand was a gift from my mother and as someone who has written a lot of projects for esp32 (this one is first public one)
esp32 is stable enough to be a usb dongle

# benefits
works almost out of the box
easy setup
low system requirements

# downsides
slow network (on esp-wroom-32 uart is used as a bridge which is VERY slow, maximum speed is about 46Kb/s which is slower than dial-up)
requires driver/program launched in the background (in case of using uart bridge)

# support

esp32:
generic esp-wroom-32

operating systems:
freebsd ^14.5 amd64

# plans
expand on linux
add support for esp32-s3 (can emulate a device so uart and driver in background is not needed)
optimise
add netblock <ip> <-- blocks requests from certain ip addresses
