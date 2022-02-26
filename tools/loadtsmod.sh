sudo killall gpm
sudo rmmod jornada720_ts
# rmb 1 - right mouse button enabled on settings symbol
# lmb 1 - left mouse button on stylus with touch event
#     2 - left mouse button on stylus, no touch event
#     3 - left mouse button on phone symbol, button lock on media player
sudo modprobe jornada720_ts mx=-735 dx=66 my=317 dy=-34 rmb=1 lmb=0
# sudo gpm -m /dev/input/event1 -t evdev
