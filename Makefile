all:
	gcc -o histext2fs main.c ext2fs_print.c 

	./histext2fs examples/example1/example1.img examples/example1/example1_state.txt examples/example1/example1_hist.txt
	./histext2fs examples/example2/example2.img examples/example2/example2_state.txt examples/example2/example2_hist.txt
	./histext2fs examples/example3/example3.img examples/example3/example3_state.txt examples/example3/example3_hist.txt
	./histext2fs examples/example4/example4.img examples/example4/example4_state.txt examples/example4/example4_hist.txt

clean:
	rm -f histext2fs 
