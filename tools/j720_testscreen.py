#!/usr/bin/python
import struct
import time
import sys

infile_path = "/dev/input/event1"

def read_coords(dev):
	FORMAT = 'llHHI'
	EVENT_SIZE = struct.calcsize(FORMAT)
	event = dev.read(EVENT_SIZE)
	x = 0
	y = 0
	sample_count = 0
	touched = True

	while event:
		(sec, usec, type, code, value) = struct.unpack(FORMAT, event)
		if type == 3: # coordinates
			sample_count += 1
			if code == 0:
				x = value
			if code == 1:
				y = value
			print ("X %u Y %u" % (x, y))
		if type == 1 and (code==273 or code==272 or code==330): # pen up / down
			if code == 330:
				print("Touch")
			if code == 272:
				print("LMB")
			if code == 273:
				print("RMB")
			if value == 0:
				print("Release")
			if value == 1:
				print("Press")

		event = dev.read(EVENT_SIZE)
	# end of while.
	return x,y

def main():
	print("Jornada 720 screen test tool")
	print("Dumping coordinates from touch device")
	raw_input("Press Enter when ready, Ctrl-C to end...")
	in_file = open(infile_path, "rb")
	ur = read_coords(in_file)

# Main program entry point.
if __name__ == "__main__":
	main()

print("End.")
