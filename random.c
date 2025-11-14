//File: random.c
#include <stdio.h>	// sscanf, printf
#include <stdlib.h>	// exit, srandom, random
int main(int argc, char *argv[])
	{
	int n=20;//default generated random numbers 
	unsigned int seed=1;//default seed
	if(argc>1)sscanf(argv[1],"%d",&n);	
	if(argc>2)sscanf(argv[2],"%u",&seed);
	srandom(seed);
	for(int i=0;i<n;i++)printf("%ld\n",random());
	exit(0);
	}//main
//End of file: random.c
