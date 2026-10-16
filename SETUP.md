# how do you even set up this thing

well its not hard as you may guess

steps to get it working:

+ clone the repo

+ make sure you have platformio installed

+ connect your esp32

+ run in the terminal: pio run -t upload <-- remove the '-t upload' if you want to just build the firmware but not flash it

+ after that run in terminal: g++ -O2 -std=c++17 host/openinetd_<OS>.cpp -o openinetd

# and if you did everything correctly and everything compiled continue on to the next step(s)

+ connect esp32 to target machine

+ use uart terminal like tio

+ run in terminal: tio -b 115200 /dev/tty<device_name>

+ reboot device and then after about a second press boot key or trigger whatever is on GPIO 0

NOTE: if you press boot key for long enough device will factory reset (forget all network configuration)

you should see 'OpenINET>' prompt on your screen

+ type 'netconf scan' and wait for scan to complete

+ when scan completes you should see your network in the list

+ type 'netconf set "<ssid>" "<password>"' double quotes are neccessary to allow spaces in the ssid/password

+ exit out of the terminal (on tio: ctrl+t q)

+ locate your compiled openinetd executable and run it in terminal (./openinetd)

after that you can either switch terminals or launch the program as background process

# done!
now setup is done and you can test network connection by typing in terminal: 'ping gnu.org' (gnu.org as an example)

have fun!
