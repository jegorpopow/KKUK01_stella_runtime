docker run -i fizruk/stella compile < examples/add2.stella > add2.c
gcc -g -DMAX_ALLOC_SIZE=$((16 * 5)) -DSTELLA_GC_STATS -std=c11 add2.c stella/runtime.c stella/gc.c -o add2
echo 3 | ./add2


docker run -i fizruk/stella compile < examples/for_flip.stella > for_flip.c
gcc -g -DMAX_ALLOC_SIZE=$((16 * 5 + 16 * 2 + 16)) -DSTELLA_GC_STATS -std=c11 for_flip.c stella/runtime.c stella/gc.c -o for_flip
echo 2 | ./for_flip

docker run -i fizruk/stella compile < examples/factorial-complex.stella > factorial-complex.c
gcc -g -DMAX_ALLOC_SIZE=$((2 * 1024 * 1024)) -DSTELLA_GC_STATS -std=c11 factorial-complex.c stella/runtime.c stella/gc.c -o factorial-complex
echo 8 | ./factorial-complex
