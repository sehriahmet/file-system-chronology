all:
	gcc -o histext2fs main.c ext2fs_print.c 
clean:
	rm -f histext2fs 
