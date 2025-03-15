# this is a minimake make file!

minimake: minimake.c
	cc -o minimake minimake.c -Wall -Wextra

minimake-tests: minimake.c vendor/utest.h
	cc -o minimake-tests minimake.c -DMINIMAKE_TESTS -Wall -Wextra
