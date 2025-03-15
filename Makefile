# this is a minimake make file!

minimake: minimake.c vendor/utest.h
	cc -o minimake minimake.c
